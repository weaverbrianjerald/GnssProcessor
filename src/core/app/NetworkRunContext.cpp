/**
 * @file NetworkRunContext.cpp
 * @brief @internal Knob snapshot + (INC-4) the relocated RTB-7 fingerprint /
 *        warm-resume restore / snapshot write and the carry-lifecycle
 *        functions of Application::runNetwork.
 *
 * Decomposition INC-4 (2026-07-16) — bodies relocated VERBATIM from
 * ApplicationNetwork.cpp; each function opens with an alias block binding the
 * former local names into the context so the moved code is textually
 * unchanged.
 */

#include "app/NetworkRunContext.hpp"

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"   // RTB-7(c) branch-faithful carry (de)serialize
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkPivotHandover.hpp"   // datumChangeAffectsKey (handover scope predicate)
#include "filter/StateKeySchema.hpp"         // StateKeys::parseSatPhaseBiasKey (harvest/stash)
#include "math/TimeUtils.hpp"
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

namespace GnssProcessor {

NetworkKnobs NetworkKnobs::snapshot(const GnssConfig& config_, const NetworkConfig& nc) {
    NetworkKnobs k;
    k.q_clock = config_.getProcessNoise().sat_clock;
    k.q_pb = config_.getProcessNoise().sat_phase_bias;
    k.q_cb = config_.getProcessNoise().sat_code_bias;
    k.pl_on = config_.networkPhaseLeveling();
    k.pl_q_amb = config_.networkPhaseLevelingQAmb();
    k.pl_warmup = config_.networkPhaseLevelingWarmupEpochs();
    k.pl_max_gap_s = config_.networkPhaseLevelingMaxGapS();
    k.pl_var_floor = config_.networkPhaseLevelingVarFloorM2();
    k.pl_warmup_on_conv = config_.networkPhaseLevelingWarmupOnConvergence();
    k.pl_refloat_on_conv = config_.networkPhaseLevelingRefloatAtConvergence();
    k.pl_conv_rms_thresh = config_.networkPhaseLevelingConvRmsThreshTecu();
    k.pb_var_floor = config_.networkSharedCarryVarFloorPbM2();
    k.cb_var_floor = config_.networkSharedCarryVarFloorCbM2();
    k.fullcov_on = config_.networkSharedCarryFullCov();
    k.carry_cpl_cond = config_.networkCarryConditionalCoupling();
    k.carry_cpl_joint = config_.networkCarryCouplingJointWhiten();
    k.carry_cpl_tu = config_.networkCarryCouplingTimeUpdate();
    k.carry_cpl_shmarg = config_.networkCarryCouplingShMarginalize();
    k.zwd_on = config_.networkCarryZwd();
    k.zwd_q = config_.networkZwdCarryQ();
    k.carry_vce = config_.networkCarryVce();
    k.ura_on = config_.networkEmitUra();
    k.ura_alpha = config_.networkUraEwmaAlpha();
    k.ura_retro = config_.networkUraRetroBump();
    k.ura_refloat_inflate = config_.networkUraRefloatInflate();
    k.raim_on = config_.networkRaim();
    k.raim_k = config_.networkRaimKSigma();
    k.raim_s0_floor = config_.networkRaimSigma0Floor();
    k.raim_min_sta = config_.networkRaimMinStations();
    k.raim_arc_persist = config_.networkRaimArcPersistEpochs();
    k.raim_maturity_gate = config_.networkRaimRefloatMaturityGate();
    k.raim_maturity_epochs = config_.networkRaimRefloatMaturityEpochs();
    k.raim_withhold = config_.networkRaimWithholdSigma0();
    k.raim_redun = config_.networkRaimRedundancyNormalize();
    k.clk_mon_on = config_.networkClockMonitor();
    k.clk_mon_abs = config_.networkClockMonitorAbsMaxM();
    k.clk_mon_jump = config_.networkClockMonitorJumpMaxM();
    k.clk_mon_couple = k.clk_mon_on && config_.networkClockMonitorCoupleRaim();
    k.clk_mon_reinit_sig = config_.networkClockMonitorReinitSigmaM();
    k.clk_mon_reinit_var = k.clk_mon_reinit_sig * k.clk_mon_reinit_sig;
    k.pivot_vr_signal = config_.networkPivotVanishReturnSignal();
    k.emit_clock_disc = config_.networkEmitClockDisc();
    k.sticky_pivot = config_.networkStickyPivot();
    k.clock_sum_to_zero = config_.networkClockDatumSumToZero();
    k.clock_brdc_anchored = config_.networkClockDatumBroadcastAnchored();
    k.clock_signal_basis_hold = config_.networkClockSignalBasisHold();
    k.cb_time_linked = config_.networkCodeDatumTimeLinked();
    k.scoped_handover = config_.networkScopedHandoverResponse();
    k.pb_excl_unmatched = config_.networkPhaseBiasExcludeUnmatchedAntenna();
    k.emit_gen_time = config_.networkEmitGenerationTime();
    k.emit_gen_id = config_.networkEmitGenerationId();
    k.ser_disc_ctrs = config_.networkSerializeDiscCounters();
    k.contract_rebroadcast_s = config_.networkContractRebroadcastS();
    k.clk_runaway_on = config_.networkClockRunawayCheck();
    k.clk_runaway_mag = config_.networkClockRunawayMagM();
    k.clk_runaway_rate = config_.networkClockRunawayRateMps();
    k.perconst_on = config_.networkPerConstWithhold();
    k.perconst_min_rows = config_.networkPerConstWithholdMinRows();
    k.emit_epoch_status = config_.networkEmitEpochStatusRecord();
    k.clk_model_on = config_.networkClockModel();
    k.clk_model_q1 = config_.networkClockModelQ1();
    k.clk_model_q2 = config_.networkClockModelQ2();
    k.clk_model_k = config_.networkClockModelKSigma();
    k.clk_model_r = config_.networkClockModelMeasStdM() * config_.networkClockModelMeasStdM();
    k.clk_model_max_gap = config_.networkClockModelMaxGapS();
    k.emit_clock_rate_on = config_.networkEmitClockRate() && !k.clk_model_on;
    k.start_offset_s = config_.networkStartOffsetHours() * 3600.0;
    k.trace_flag_rates = nc.trace_flag_rates;
    k.phase_datum_ensemble = config_.networkPhaseDatumEnsemble();
    k.phase_datum_eq34 = config_.networkPhaseDatumEq34();
    k.carry_invariant = k.phase_datum_eq34 && config_.networkCarryInvariant();
    k.eq34_rpb_prior_sigma = config_.networkPhaseDatumEq34RpbPriorSigmaM();
    k.eq34_rpb_prior_w = (k.phase_datum_eq34 && k.eq34_rpb_prior_sigma > 0.0) ? 1.0 / (k.eq34_rpb_prior_sigma * k.eq34_rpb_prior_sigma) : 0.0;
    k.eq34_rpb_q = (k.phase_datum_eq34 && !k.carry_invariant) ? config_.networkPhaseDatumEq34RpbQ() : 0.0;
    k.eq34_shift_on = k.phase_datum_eq34 && config_.networkPhaseDatumEq34CarryShift() && !k.carry_invariant;
    k.eq34_longevity = k.phase_datum_eq34 && config_.networkPhaseDatumEq34LongevityElection();
    k.eq34_extend_only = k.phase_datum_eq34 && config_.networkPhaseDatumEq34ExtendOnly();
    k.phase_datum_prioritized = config_.networkPhaseDatumPrioritized() || k.phase_datum_ensemble || k.phase_datum_eq34;
    k.phase_hold_sigma = k.phase_datum_ensemble ? config_.networkPhaseDatumHoldSigmaM() : 0.0;
    k.phase_hold_on = k.phase_hold_sigma > 0.0;
    k.phase_hold_var = k.phase_hold_sigma * k.phase_hold_sigma;
    return k;
}

void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                const std::vector<NetStation>& stations) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    const auto& nc = config_.getNetworkConfig();
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& snap_path = ctx.carry.snap_path;
    auto& live_fingerprint = ctx.carry.live_fingerprint;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const bool   phase_datum_ensemble = ctx.knobs.phase_datum_ensemble;
    const bool   phase_datum_eq34 = ctx.knobs.phase_datum_eq34;
    const bool   carry_invariant = ctx.knobs.carry_invariant;
    const bool   pl_on = ctx.knobs.pl_on;
    const double pl_q_amb = ctx.knobs.pl_q_amb;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor = ctx.knobs.pl_var_floor;
    const double pb_var_floor = ctx.knobs.pb_var_floor;
    const double cb_var_floor = ctx.knobs.cb_var_floor;
    const bool   zwd_on = ctx.knobs.zwd_on;
    const double zwd_q = ctx.knobs.zwd_q;
    const bool   clk_model_on = ctx.knobs.clk_model_on;
    const double clk_model_q1 = ctx.knobs.clk_model_q1;
    const double clk_model_q2 = ctx.knobs.clk_model_q2;
    const double clk_model_k = ctx.knobs.clk_model_k;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   ura_on = ctx.knobs.ura_on;
    const double ura_alpha = ctx.knobs.ura_alpha;
    const bool   ura_retro = ctx.knobs.ura_retro;
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate;
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;
    const bool   sticky_pivot = ctx.knobs.sticky_pivot;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    const bool   emit_gen_id = ctx.knobs.emit_gen_id;
    const double q_clock = ctx.knobs.q_clock;
    const double q_pb = ctx.knobs.q_pb;
    const double q_cb = ctx.knobs.q_cb;
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored;
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold;
    const bool   pb_excl_unmatched = ctx.knobs.pb_excl_unmatched;
    const bool   cb_time_linked = ctx.knobs.cb_time_linked;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   carry_cpl_tu = ctx.knobs.carry_cpl_tu;
    const bool   carry_cpl_shmarg = ctx.knobs.carry_cpl_shmarg;
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint;

    if (!snap_path.empty()) {
        // Config fingerprint over the DATUM-relevant fields: resuming under a
        // changed S-basis/pivot pin, station roster (order defines the carry's
        // station indices), carry q's / gap bounds / floors, clock-model
        // parameters, or emitter channel config must COLD-start — the carried
        // state would be interpreted in the wrong frame or under the wrong rules.
        std::string fp;
        auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
        for (const auto& st : stations) { add(st.name); add(st.is_pivot ? "P" : "-"); }
        add(nc.clock_datum_station); add(nc.clock_datum_satellite);
        // The phase-bias gauge realization (single-pivot vs prioritized anchor)
        // defines which station's ambiguities carry the datum; a resume across a
        // toggle change would interpret the carried anchors in the wrong frame.
        add(fmt::format("{}", phase_datum_prioritized));
        // Fix A: the anchor-free ensemble datum references the receiver ENSEMBLE
        // (Σ_r z=0), a different carried-ambiguity frame than the single-anchor
        // pivot/prioritized datum (both of which set phase_datum_prioritized).
        // Appended ONLY when armed, so every pre-existing (non-ensemble) snapshot
        // fingerprint is unchanged (byte-identical).
        if (phase_datum_ensemble) add("phdatum:ens");
        // Card 3: the Eq-34 chain datum references a different carried-ambiguity
        // frame (per-receiver RPB split + spanning-tree pins) than the anchor/pivot
        // or ensemble datums; a resume across a toggle flip must cold-start.
        // Appended ONLY when armed, so every non-eq34 snapshot stays byte-identical.
        if (phase_datum_eq34) add("phdatum:eq34");
        // Card 4 (M1) OPEN-4: the invariant carry snapshots a DD z-store the legacy
        // carries don't have; a resume across a carry_invariant flip must cold-start.
        // Appended ONLY when armed, so every non-carry snapshot stays byte-identical.
        if (carry_invariant) add("carry:inv");
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        add(fmt::format("{}", pl_var_floor));    add(fmt::format("{}", pb_var_floor));
        add(fmt::format("{}", cb_var_floor));    add(fmt::format("{}", zwd_on));
        add(fmt::format("{}", zwd_q));           add(fmt::format("{}", clk_model_on));
        add(fmt::format("{}", clk_model_q1));    add(fmt::format("{}", clk_model_q2));
        add(fmt::format("{}", clk_model_k));     add(fmt::format("{}", clk_model_max_gap));
        add(fmt::format("{}", config_.networkEmitClockRate()));
        add(fmt::format("{}", ura_on));          add(fmt::format("{}", ura_alpha));
        add(fmt::format("{}", ura_retro));       add(fmt::format("{}", ura_refloat_inflate));
        add(fmt::format("{}", emit_clock_disc));
        add(fmt::format("{}", config_.networkEmitterStalenessHorizonS()));
        add(fmt::format("{}", sticky_pivot));    add(fmt::format("{}", scoped_handover));
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // Card 6: apriori_source + anchor σ are datum-relevant.  The carried clock
        // frame is Δδtˢ-vs-broadcast in brdc_rts but Δδtˢ-vs-precise in sp3 (differ by
        // ~metres), and only brdc_rts carries the relevel_iode baseline — so a warm
        // resume must NOT cross an apriori_source change or it would fold a clock carry
        // in the wrong frame and re-level against a stale baseline.
        add(fmt::format("{}", static_cast<int>(config_.networkAprioriSource())));
        add(fmt::format("{}", config_.networkAprioriClockAnchorSigmaM()));
        // The clock-gauge REALIZATION defines the frame the carried clock states
        // live in (pin / sum-to-zero / broadcast-anchored differ by per-const or
        // common-mode constants) — a resume across a realization change must
        // cold-start.  Appended ONLY when a non-default realization is armed so
        // every pre-existing (pin-mode) snapshot fingerprint is unchanged.
        if (clock_sum_to_zero || clock_brdc_anchored)
            add(fmt::format("clkdatum:s2z={},bca={}", clock_sum_to_zero,
                            clock_brdc_anchored));
        // Optional emitter discontinuity channels are construction parameters of
        // the snapshotted emitter state (restoreState hard-fails on a mismatch) —
        // same conditional-append rule keeps default fingerprints unchanged.
        if (config_.networkCodeBiasDiscJumpTest() || config_.networkClockDiscOnSetChange())
            add(fmt::format("emitchan:cbj={},cks={}", config_.networkCodeBiasDiscJumpTest(),
                            config_.networkClockDiscOnSetChange()));
        // The held clock signal basis defines which IF combination each carried
        // clock state is referenced to; a resume across a toggle change would carry
        // clock states in a re-selected-per-epoch frame vs a held frame.  Appended
        // ONLY when armed so every pre-existing snapshot fingerprint is unchanged.
        if (clock_signal_basis_hold)
            add(fmt::format("clkbasishold={}", clock_signal_basis_hold));
        // Excluding unmatched-antenna stations from phase-bias emission changes which
        // per-(sat,signal) phase-bias states the carried solution holds; a resume across a
        // toggle change would carry a different shared-state set.  Appended ONLY when armed
        // so every pre-existing snapshot fingerprint is unchanged.
        if (pb_excl_unmatched)
            add(fmt::format("pbexclunmatched={}", pb_excl_unmatched));
        // T2e: epoch decimation changes the carried-state cadence, so a resume must match it.
        // Conditional-append (only when != 1) keeps every existing config's fingerprint byte-
        // identical.  The DD-trace stride is TRACE-ONLY and deliberately NOT folded in — arming
        // a trace on resume must not invalidate an untraced snapshot.
        if (nc.epoch_decimation_n != 1)
            add(fmt::format("edecim={}", nc.epoch_decimation_n));
        // T5: the GM deviation changes the estimation (not trace-only), and its carry
        // is NOT persisted in the snapshot — a resume across a tau change must cold-start.
        if (shc.rep_error_tau_s > 0.0)
            add(fmt::format("shdevtau={}", shc.rep_error_tau_s));
        // Iono-2 dual-layer: the second shell adds carried SH coefficients whose
        // meaning depends on its degree AND height (a resume across a re-layering
        // would fold layer-fitted coefficients into a different shell geometry).
        // Conditional-append (only when armed) keeps every existing fingerprint
        // byte-identical.
        if (shc.layer2_max_degree > 0)
            add(fmt::format("shl2={}@{}", shc.layer2_max_degree, shc.layer2_height_km));
        // Card 1: the conditional local carry changes what the per-station carry MEANS
        // (a shared-coupled conditional row vs an independent diagonal marginal) and the
        // per-column coupling data (M_r rows, ŝ_prev, P_prev) is NOT persisted in the v1
        // snapshot — a resume across a toggle change would fold a carry of the wrong kind.
        // Conditional-append (only when armed) keeps every existing fingerprint byte-identical.
        if (carry_cpl_cond)
            add(fmt::format("carrycplcond={}", carry_cpl_cond));
        // Card 2: the time-linked code-bias datum changes what the emitted OSB/clock
        // split MEANS (a time-continuity datum vs a per-epoch set-mean datum) and its
        // per-(const,signal) previous-value map is NOT persisted in the snapshot — a
        // resume across a toggle change would compare a solve in one datum against a
        // carry in the other.  Conditional-append (only when armed) keeps every
        // existing fingerprint byte-identical.
        if (cb_time_linked)
            add(fmt::format("cbtimelink={}", cb_time_linked));
        // Card 1d: the border time update changes what an injected coupling row MEANS
        // (a κ-shrunk, re-independenced pseudo-obs of the MOVED border vs the raw
        // harvest-epoch row) and the κ inputs (per-column P_prev + q ladder) are not
        // persisted in the v1 snapshot.  Conditional-append (only when armed) keeps
        // every existing fingerprint byte-identical.
        if (carry_cpl_tu)
            add(fmt::format("carrycpltu={}", carry_cpl_tu));
        // Card 1d-SH: marginalized-vs-carried SH coupling changes what an armed carry
        // MEANS; conditional-append keeps existing fingerprints byte-identical.
        if (carry_cpl_shmarg)
            add(fmt::format("cplshmarg={}", carry_cpl_shmarg));
        // Card 1b: JOINT-whitening reshapes the injected coupling rows (departed cols →
        // shared M_w marginalization vs a per-row scalar σ²) — a solve-determining
        // treatment on the SAME footing as carry_cpl_cond/_tu/_shmarg above, all of
        // which gate a resume.  It was the lone Card-1 coupling knob missing here, so a
        // warm resume flipping joint-whiten (with carry_cpl_cond still ON) matched
        // fingerprints and was permitted to proceed and diverge.  Conditional-append
        // (only when armed) keeps every non-joint fingerprint byte-identical; joint
        // requires carry_cpl_cond so an armed-joint config already carries carrycplcond.
        if (carry_cpl_joint)
            add(fmt::format("carrycpljoint={}", carry_cpl_joint));
        // SPEC_day129_frame_guards Phase 2.1: the CB-excursion guard changes what a
        // carried code bias MEANS across an excursion (a reinit-decorrelated state
        // vs a stiff continuation) and its pending-bump queue is not snapshotted —
        // a resume flipping it would fold a carry of the wrong kind and could
        // silently drop a queued discontinuity.  On the SAME footing as
        // cbtimelink / carrycpl* above.  Conditional-append (only when armed)
        // keeps every existing (unarmed) fingerprint byte-identical.
        if (nc.cb_excursion_reinit)
            add(fmt::format("cbexc={},{}", nc.cb_excursion_median_m,
                            nc.cb_excursion_reinit_all_sats));
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
    }
}

void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                          const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                          NetworkInvariantCarryDriver& carry_driver,
                          const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    const auto& nc = config_.getNetworkConfig();
    const auto& synced = sync.getSyncedEpochs();
    auto& snap_path = ctx.carry.snap_path;
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
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& retro_active = ctx.carry.retro_active;
    auto& relevel_iode = ctx.carry.relevel_iode;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
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

    // RTB-7(d) HB-1: when state_snapshot_require is armed, ANY failure to complete a
    // warm resume is a hard run error instead of a warn + silent cold start — the
    // structural guard for same-snapshot A/B (a resume overwrites the snapshot it read,
    // so an un-re-seeded relaunch would otherwise be graded as warm while running cold).
    auto hb1 = [&](const std::string& why) {
        if (nc.state_snapshot_require)
            throw std::runtime_error(
                "Network (RTB-7d/HB-1): state_snapshot_require is armed but the warm resume "
                "did not complete: " + why);
    };
    if (nc.state_snapshot_require && !snap_path.empty()
        && !std::filesystem::exists(snap_path))
        hb1("snapshot file '" + snap_path.string() + "' does not exist");
    if (!snap_path.empty() && std::filesystem::exists(snap_path)) {
        NetworkStateSnapshot snap;
        std::string err;
        if (!NetworkStateSnapshotIO::read(snap_path, snap, &err)) {
            log->warn("Network (RTB-7): state snapshot '{}' failed to parse ({}) — COLD START "
                      "(a new generation id is minted at first emit as usual).",
                      snap_path.string(), err);
            hb1("parse failure: " + err);
        } else {
            // First epoch this run will actually process (honouring the
            // start_offset_hours fast-forward) — the age reference of the gate.
            double first_live_s = -1.0;
            if (!synced.empty()) {
                const double anchor_s = TimeUtils::continuousGpsSeconds(synced.front().time);
                for (const auto& sf : synced) {
                    const double t_s = TimeUtils::continuousGpsSeconds(sf.time);
                    if (start_offset_s <= 0.0 || (t_s - anchor_s) >= start_offset_s) {
                        first_live_s = t_s;
                        break;
                    }
                }
            }
            // Carry-validity horizon = max of the per-key gap bounds.  Past it
            // every restored key would be dropped/reseeded downstream anyway;
            // WITHIN it, per-key staleness after restore is handled by the
            // EXISTING machinery (local-carry max-gap drop, D2 q·dt inflation
            // over the true gap via the restored prev_week/prev_tow, clock-model
            // per-sat t_last gap logic) — restore everything, let the
            // established per-key logic judge each key.
            const double horizon_s = std::max(pl_max_gap_s, clk_model_max_gap);
            std::string gate;
            if (first_live_s < 0.0) {
                log->warn("Network (RTB-7): state snapshot '{}' found but there are no live "
                          "epochs to resume against — COLD START.", snap_path.string());
                hb1("no live epochs to resume against");
            } else if (!NetworkStateSnapshotIO::warmResumeAllowed(
                           snap, live_fingerprint, first_live_s, horizon_s, &gate)) {
                log->warn("Network (RTB-7): state snapshot '{}' NOT warm-resumable — {} — "
                          "COLD START (a new generation id is minted at first emit as usual).",
                          snap_path.string(), gate);
                hb1("gate rejected the snapshot: " + gate);
            } else {
                // Emitter layer FIRST (all-or-nothing): a failed emitter restore
                // discards the whole warm resume — restoring the driver carries
                // without the emitter's baselines/pending flags would emit WRONG
                // discontinuity counters (the silent consumer datum break).
                std::istringstream es(snap.emitter_state);
                if (!emitter.restoreState(es)) {
                    log->warn("Network (RTB-7): state snapshot '{}' passed the gate but its "
                              "EMITTER section failed to restore (corrupt or parameter "
                              "mismatch) — COLD START; driver state deliberately NOT "
                              "restored (the two layers only make sense together).",
                              snap_path.string());
                    hb1("emitter section failed to restore");
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
                        // Iono-2 dual-layer: the second shell's coefficients ride the
                        // (empty-for-VtecSh) sat field as "L1" — a layer tag old
                        // readers key-mismatch into a safe climatology reseed.
                        if (cp.kind == SharedParamKind::VtecSh && cp.sat == "L1") {
                            cp.sh_layer = 1;
                            cp.sat.clear();
                        }
                        prev_carry.push_back(std::move(cp));
                    }
                    // C1: the v1 snapshot serializes per-key (value, var) only — the
                    // dense covariance block deliberately is not persisted.  The first
                    // post-resume fold therefore runs the diagonal path (identical
                    // legacy semantics); the dense block re-seeds at the first emit.
                    if (fullcov_on && !prev_carry.empty())
                        log->info("Network (C1): warm resume restored {} diagonal carries — "
                                  "the full-covariance carry re-seeds at the first emitted "
                                  "epoch.", prev_carry.size());
                    prev_pivot    = snap.prev_pivot;
                    last_pivot    = snap.last_pivot;
                    c1_zoh_active = snap.c1_zoh_active;
                    prev_clk      = snap.prev_clk;
                    clock_osc.clear();
                    for (const auto& [sat, o] : snap.clock_osc) {
                        ClockOsc c;
                        c.x0 = o.x0; c.x1 = o.x1;
                        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
                        c.t_last = o.t_last; c.init = o.init;
                        clock_osc[sat] = c;
                    }
                    clock_rate_est.clear();
                    for (const auto& [sat, o] : snap.clock_rate_est) {   // network.emit_clock_rate decoupled C1 state
                        ClockOsc c;
                        c.x0 = o.x0; c.x1 = o.x1;
                        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
                        c.t_last = o.t_last; c.init = o.init;
                        clock_rate_est[sat] = c;
                    }
                    local_carry.clear();
                    for (const auto& [key, l] : snap.local_carry)
                        local_carry[key] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    zwd_carry.clear();
                    for (const auto& [idx, l] : snap.zwd_carry)
                        zwd_carry[idx] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    ura_sigma0_ewma     = snap.ura_sigma0_ewma;
                    ura_refloat_horizon = snap.ura_refloat_horizon;
                    ura_retro_sats      = snap.ura_retro_sats;
                    retro_active        = snap.retro_active;
                    // Card 6: restore the eph-cutover re-level baseline + any pending
                    // forced disc so a warm resume does not re-level against a stale
                    // baseline (or lose a pending forced bump).  Empty in an sp3 or
                    // pre-Card-6 snapshot ⇒ the first brdc_rts epoch re-seeds cleanly.
                    relevel_iode             = snap.relevel_iode;
                    eph_cutover_disc_pending = snap.eph_cutover_disc_pending;
                    // Card 2: the time-linked code-bias previous-value map is NOT
                    // persisted, so it re-seeds empty here.  A satellite emitted before
                    // the snapshot whose split now re-seeds IS a re-reference the
                    // consumer must see — mark the run "resumed" so every first post-
                    // resume re-seed disc-bumps (conservative; a genuinely-new sat's
                    // first emit absorbs a harmless bump).  Inert unless code_datum_time_linked.
                    code_bias_datum_resumed_ = true;
                    // RTA-3 seam (the critical one): PRESERVE the snapshot's
                    // generation id.  The first-emit minting site only mints when
                    // generation_id == 0, so a restored non-zero id SUPPRESSES the
                    // mint — consumers keep the same product datum and never
                    // hard-reset.  (Same-config guarantee: emit_gen_id is in the
                    // fingerprint, so an id-less snapshot only warm-resumes an
                    // id-less run, where 0 is correct.)
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
                    // RTB-7(b): ramp state restores — a warm-resumed producer is
                    // NOT degraded.  Already-lifted ⇒ grade 0 ("warm"); mid-ramp
                    // ⇒ the ramp CONTINUES from the restored counts (never
                    // restarts), with the grade anchored at the original first
                    // emit (honest total product time to grade).
                    ura_ewma_updates     = static_cast<size_t>(snap.ura_ewma_updates);
                    ramp_lifted          = snap.ramp_lifted;
                    first_emit_content_s = snap.first_emit_content_s;
                    if (nc.warmup_ura_floor_m > 0.0 && ura_on && ramp_lifted) {
                        grade_metric_s = 0.0;
                        log->info("Network (RTB-7): cold-start-to-product-grade: 0 s (warm "
                                  "resume — ramp already lifted before the bounce).");
                    }
                    // RTB-7(c): branch-faithful — restore the invariant DD carry (the
                    // z-store branch) + the ArcKey vertex numbering it references.  Only
                    // when armed AND the blob is present; a failure cold-starts the carry
                    // (shared/emitter state stay warm) so the resume degrades to RTB-7(a).
                    if (nc.state_snapshot_carry && !snap.carry_state.empty()) {
                        ctx.carry.carry_sat_vertex = snap.carry_sat_vertex;
                        ctx.carry.carry_group_id.clear();
                        for (const auto& gg : snap.carry_group_id)
                            ctx.carry.carry_group_id[{std::get<0>(gg), std::get<1>(gg)}] =
                                std::get<2>(gg);
                        std::istringstream cs(snap.carry_state);
                        std::string cerr;
                        bool carry_ok = carry_driver.restoreCarry(cs, &cerr);
                        if (carry_ok) {
                            // RE-4: the blob is numbered in the WRITING run's id space —
                            // reject it unless every chord/arc binds to these live maps,
                            // rather than silently re-keying chords onto wrong satellites.
                            std::set<int> satv, gids;
                            for (const auto& sv : ctx.carry.carry_sat_vertex) satv.insert(sv.second);
                            for (const auto& gi : ctx.carry.carry_group_id)   gids.insert(gi.second);
                            carry_ok = carry_driver.validateCarryBinding(
                                static_cast<int>(nc.stations.size()), satv, gids, &cerr);
                        }
                        if (!carry_ok) {
                            log->warn("Network (RTB-7c): carry blob present but NOT usable ({}) "
                                      "— the invariant carry COLD-STARTS (branch not "
                                      "reproduced; shared/emitter state stay warm).", cerr);
                            carry_driver.reset();
                            ctx.carry.carry_sat_vertex.clear();
                            ctx.carry.carry_group_id.clear();
                            if (nc.state_snapshot_require)
                                throw std::runtime_error(
                                    "Network (RTB-7d/HB-1): state_snapshot_require is armed and "
                                    "the carry blob could not be restored/bound: " + cerr);
                        } else {
                            // RE-2: wired event response for the stiff restored carry.
                            std::size_t shocked = 0;
                            if (nc.state_snapshot_resume_shock_m > 0.0)
                                shocked = carry_driver.applyResumeShock(
                                    nc.state_snapshot_resume_shock_m
                                    * nc.state_snapshot_resume_shock_m);
                            log->info("Network (RTB-7c): BRANCH-FAITHFUL — restored invariant "
                                      "DD carry ({} carried DD), {} sat-vertex + {} group-id map "
                                      "entries; sigma0 {}; resume shock {}.  Pᶻ shape: {}.",
                                      carry_driver.resumeCarryChords(),
                                      snap.carry_sat_vertex.size(), snap.carry_group_id.size(),
                                      carry_driver.resumeSigma0Seeded()
                                          ? fmt::format("SEEDED {:.4f}",
                                                        carry_driver.resumeSigma0Value())
                                          : std::string("not seeded (v1 blob / calib off)"),
                                      shocked ? fmt::format("sigma={:.3f} m on {} block(s)",
                                                            nc.state_snapshot_resume_shock_m,
                                                            shocked)
                                              : std::string("none"),
                                      // RTB-7(e) / invariant #4: a diagonal restore looks
                                      // identical to a dense one everywhere else.
                                      carry_driver.resumeCarryDiagonal()
                                          ? fmt::format("DIAGONAL-ONLY (v3 blob, {} block(s)) — "
                                                        "means exact, chord cross-covariances "
                                                        "start at ZERO and re-earn from data",
                                                        carry_driver.resumeCarryDiagonalBlocks())
                                          : std::string("dense (v1/v2 blob) — the branch "
                                                        "geometry resumes exactly"));
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
                              horizon_s, generation_id, prev_carry.size(), local_carry.size(),
                              zwd_carry.size(), clock_osc.size(), prev_clk.size(),
                              prev_pivot.size(), last_pivot.size(), ura_retro_sats.size(),
                              ura_refloat_horizon.size(), n_emitted);
                }
            }
        }
    }
}

void networkWriteStateSnapshot(NetworkRunContext& ctx, const GnssConfig& config_,
                               const GnssTime& t, double pl_now_s,
                               NetworkProductEmitter& emitter,
                               const NetworkInvariantCarryDriver& carry_driver,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
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

    // --- RTB-7(a): periodic atomic state snapshot -------------------------
    // End-of-epoch, EMITTED epochs only (skipped epochs `continue` above and
    // mutate no carry state), every state_snapshot_interval_s of PRODUCT
    // time.  Written AFTER this epoch's product emit + flush, to the
    // snapshot's own temp file + rename — the product writer is never
    // flushed or reordered by this block (emission is unperturbed).  Path
    // empty (default) ⇒ byte-identical.
    if (!snap_path.empty()
        && (last_snapshot_write_s < 0.0
            || pl_now_s - last_snapshot_write_s >= nc.state_snapshot_interval_s)) {
        // RTB-7 cold-state exclusions (in-tree record of a decision previously
        // documented only in memory/report): this snapshot carries ONLY shared,
        // network-level producer state — the cross-epoch carries (phase-level /
        // iono / ZWD), clock oscillators, pivots, generation id, URA/ramp metrics
        // and the emitter's disc-counter / baseline / first-sighting bookkeeping.
        // Per-STATION filter state is DELIBERATELY NOT snapshotted: per-station
        // cycle-slip arcs and slip-detector history, per-station StateManager
        // ambiguity / iono / receiver-bias marks, and per-station covariance.  On
        // a warm resume those layers cold-start and re-warm within a few epochs,
        // and the post-fit RAIM safety net absorbs the transient — serializing the
        // large per-station covariance blocks would buy nothing the re-warm does
        // not already provide.  That is the recovery-budget trade RTB-7 accepts.
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
            // Iono-2 dual-layer: tag the second shell in the (empty-for-VtecSh)
            // sat field — record arity unchanged, old readers reseed safely.
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
        for (const auto& [key, lc] : local_carry)
            snap.local_carry[key] =
                NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
        for (const auto& [idx, lc] : zwd_carry)
            snap.zwd_carry[idx] =
                NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
        snap.ura_refloat_horizon = ura_refloat_horizon;
        snap.ura_retro_sats      = ura_retro_sats;
        snap.retro_active        = retro_active;
        snap.relevel_iode              = relevel_iode;              // Card 6
        snap.eph_cutover_disc_pending  = eph_cutover_disc_pending;  // Card 6
        // RTB-7(c): branch-faithful — serialize the invariant DD carry (z-store branch)
        // + its ArcKey vertex numbering (sat→vertex, (const,signal)→group), so a warm
        // resume reproduces the phase-ambiguity branch instead of re-converging one.
        if (nc.state_snapshot_carry) {
            snap.carry_sat_vertex = ctx.carry.carry_sat_vertex;
            snap.carry_group_id.clear();
            snap.carry_group_id.reserve(ctx.carry.carry_group_id.size());
            for (const auto& [key, gid] : ctx.carry.carry_group_id)
                snap.carry_group_id.emplace_back(key.first, key.second, gid);
            std::ostringstream ccs;
            // RTB-7(e): write-side shape knob.  The blob is ~99% of the snapshot at scale
            // (305 sta: 13.1M dense Pᶻ numbers vs 7.9k means), so the diagonal shape is the
            // difference between a 258 MB and a ~160 KB file.  Read side is unaffected.
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
            ++ctx.carry.snap_writes;   // RTB-7(e): run-end blob-size readout (invariant #4)
        // RTB-7(d) BR-1: keep a per-epoch copy so the branch can be graded over time
        // (scripts/diff_carry_blobs.py).  Write-side only — same atomic temp+rename, and
        // the estimation cannot see it (this runs after emit over const refs).
        if (nc.state_snapshot_keep_history) {
            std::filesystem::path hp = snap_path;
            hp += fmt::format(".ep{:04d}", static_cast<unsigned long long>(snap.n_emitted));
            std::string herr;
            if (!NetworkStateSnapshotIO::write(hp, snap, &herr))
                log->warn("Network (RTB-7d/BR-1): history snapshot write to '{}' failed ({}).",
                          hp.string(), herr);
        }
        last_snapshot_write_s = pl_now_s;
    }
}

void networkInjectShVtecPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                              const GnssConfig& config_, NetworkEstimator& est,
                              const SphericalHarmonicIonosphere* sh_model,
                              const SphericalHarmonicIonosphere* sh_model2,
                              bool datum_changed, double dt, bool within_epoch_armed) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& prev_carry = ctx.carry.prev_carry;
    auto& cap_priors = ws.cap_priors;

    // --- SH-VTEC prior (Kaula + temporal), folded BEFORE the gate ----------
    // The SH coefficients are SHARED columns in the reduced normals; without a
    // bound on every one, a coefficient whose IPP support rotated over an
    // ocean/polar gap is near-null and trips the all-or-nothing rank gate
    // (design §6.1).  A degree-dependent Kaula prior (σ_n = kaula_scale/n; a₀₀
    // a LOOSE climatology prior so the mean VTEC stays data-determined and is
    // NOT clamped to an external value, §6.2) keeps every coefficient PSD-bounded
    // → declared_dimension stays = #constellations regardless of coverage.  The
    // SAME prior carries the field forward in time as a bounded random walk
    // (anchor the previous estimate, inflate by the per-degree q, but never
    // exceed the Kaula bound).  Folded via addSharedPrior BEFORE the gate (so the
    // gate sees the regularized SH block) — unlike the clock/bias D2 carry-
    // forward, which must stay AFTER the gate (it would otherwise fill the clock
    // gauge the datum pins).
    if (config_.estimateNetworkIonoSh() && sh_model && est.numShared() > 0) {
        std::map<std::tuple<int, int, int, char>, const CarryPrior*> sh_carry;
        // RTA-2 scope decision: the SH-VTEC carry skip deliberately stays
        // GLOBAL even under network.scoped_handover_response.  The SH
        // coefficients are constellation-LESS shared columns, so no scoped
        // subset exists for them; and although the pure clock-gauge
        // S-transform leaves the iono columns invariant in exact arithmetic,
        // the carried SH posterior was estimated JOINTLY with the old-datum
        // clock/bias states, so the conservative (existing) response is kept:
        // on ANY datum change the RW-toward-previous term is dropped for one
        // epoch and the coefficients fall back to the always-on Kaula
        // climatology anchor below (still PSD-bounded, gate-safe) — a cheap,
        // bounded cost that avoids scoping a column that belongs to no
        // constellation.
        if (!datum_changed)
            for (const CarryPrior& cp : prev_carry)
                if (cp.kind == SharedParamKind::VtecSh)
                    sh_carry.emplace(
                        std::make_tuple(cp.sh_layer, cp.sh_n, cp.sh_m, cp.sh_trig), &cp);

        // Dual-layer (Iono-2): the SAME mean-reverting prior recipe per shell,
        // with per-layer Kaula scale / q / a₀₀ anchor.  The layer-2 a₀₀ anchor
        // is deliberately TIGHT (the inter-layer split of the mean VTEC is
        // quasi-null from ground-only geometry — only the mapping-function
        // difference of the two shell heights observes it); the TOTAL mean
        // stays data-determined through layer 1's loose anchor.
        std::vector<NetworkEstimator::SharedPrior> sh_priors;
        for (const ShLayer& lay :
             shLayers(shc, sh_model, sh_model2)) {
            sh_priors.reserve(sh_priors.size() + lay.model->coeffs().size());
            for (const auto& c : lay.model->coeffs()) {
                const double kaula_sigma = (c.n == 0)
                    ? lay.mean_prior_sigma_tecu
                    : lay.kaula_scale_tecu / static_cast<double>(c.n);
                const double kaula_var = kaula_sigma * kaula_sigma;
                const double clim = (c.n == 0 && c.m == 0 && c.trig == 'C')
                    ? lay.mean_prior_tecu : 0.0;   // a₀₀ → mean VTEC; others → 0

                // Mean-reverting (first-order Gauss-Markov) prior: ALWAYS apply the
                // Kaula climatology anchor (toward clim, σ = kaula_sigma) AND, when a
                // posterior was carried forward, a random-walk anchor (toward the
                // previous estimate, inflated by the per-degree q).  Combining them in
                // information space pulls every coefficient back toward climatology
                // each epoch (so the field cannot random-walk away — the design's
                // "unobserved coefficients are pulled to their prior", §6.1) while the
                // RW term preserves temporal continuity for well-observed ones.  A
                // pure RW-toward-previous (no climatology re-assertion) let the global
                // field drift to unphysical magnitudes; this is the bound.
                double info  = 1.0 / kaula_var;       // climatology Tikhonov (always on)
                double sumiv = info * clim;
                const auto it =
                    sh_carry.find(std::make_tuple(lay.layer, c.n, c.m, c.trig));
                if (it != sh_carry.end() && std::isfinite(it->second->value)
                    && it->second->var > 0.0) {
                    const double q_deg = (c.n == 0)
                        ? lay.q_per_degree
                        : lay.q_per_degree / static_cast<double>(c.n * c.n);
                    const double var_rw = it->second->var + q_deg * std::max(0.0, dt);
                    if (var_rw > 0.0) {
                        const double info_rw = 1.0 / var_rw;
                        info  += info_rw;
                        sumiv += info_rw * it->second->value;
                    }
                }
                if (!(info > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::VtecSh;
                p.sh_n = c.n; p.sh_m = c.m; p.sh_trig = c.trig; p.sh_layer = lay.layer;
                p.value = sumiv / info; p.info = info;
                sh_priors.push_back(p);
            }
        }
        est.addSharedPrior(sh_priors);
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), sh_priors.begin(), sh_priors.end());
    }
}

void networkInjectPhaseHoldPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                 NetworkEstimator& est, bool within_epoch_armed) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    const bool   phase_hold_on = ctx.knobs.phase_hold_on;
    const double phase_hold_var = ctx.knobs.phase_hold_var;
    auto& cap_priors = ws.cap_priors;

    // --- Fix A hold: held satellite phase-bias datum (seed-once + time-lock) ---
    // Inject the held b_s priors BEFORE the null-space gate — like the SH-VTEC
    // prior above, and UNLIKE the clock/code D2 carry below (which must stay
    // post-gate so the gate sees the data-only clock-gauge structure).  A held
    // (sat,signal) has its Σz=0 fold SKIPPED in buildEnsembleGauges (a hard w→∞
    // fold would override a finite prior), so this stiff fixed-variance prior is
    // that column's DATUM: it must be in the reduced normals the gate inspects, or
    // the held column reads as a near-null gauge and trips the O(n³) heal / a
    // withhold every epoch.  (In practice the tight local ambiguity carry usually
    // already lifts it above the cutoff, but this makes the datum explicit and
    // robust to a weak/cold carry on a fresh arc.)  The phase-ensemble datum is
    // orthogonal to the clock-pivot datum_changed handover, so it is injected
    // EVERY epoch regardless.  Phase biases are excluded from the clock-frame D2
    // carry loops below under hold, so this is the single injection (no double
    // fold).  Inert / no-op unless phase_hold_on (byte-identical).
    if (phase_hold_on && !prev_carry.empty()) {
        std::vector<NetworkEstimator::SharedPrior> hp;
        hp.reserve(prev_carry.size());
        for (const CarryPrior& cp : prev_carry) {
            if (cp.kind != SharedParamKind::PhaseBias) continue;
            if (!(cp.var > 0.0)) continue;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / cp.var;
            hp.push_back(p);
        }
        est.addSharedPrior(hp);  // skips keys absent from this epoch's layout
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
    }
}

void networkInjectD2CarryPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                               NetworkRunTraces& traces, const GnssTime& t,
                               NetworkEstimator& est, bool datum_changed,
                               const std::set<char>& changed_consts, double dt,
                               bool within_epoch_armed,
                               const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    auto& eq34_reref = ctx.carry.eq34_reref;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    const bool   fullcov_on = ctx.knobs.fullcov_on;
    const bool   phase_hold_on = ctx.knobs.phase_hold_on;
    const bool   clk_model_on = ctx.knobs.clk_model_on;
    const double q_clock = ctx.knobs.q_clock;
    const double q_cb = ctx.knobs.q_cb;
    const double q_pb = ctx.knobs.q_pb;
    const double clk_mon_reinit_var = ctx.knobs.clk_mon_reinit_var;
    const double pb_var_floor = ctx.knobs.pb_var_floor;
    const double cb_var_floor = ctx.knobs.cb_var_floor;
    auto& cap_priors = ws.cap_priors;
    auto& cap_dense_keys = ws.cap_dense_keys;
    auto& cap_dense_cov = ws.cap_dense_cov;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // RTA-2 scope predicate — re-declared verbatim over the explicit
    // (datum_changed, changed_consts) parameters; identical read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };

    // --- D2 carry-forward prior (diagonal, inflated over the gap) -------
    // Applied AFTER the gate (which must see the DATA-only gauge structure)
    // and BEFORE the solve.  The prior anchors each non-datum shared
    // parameter to its previous-epoch estimate; it is skipped on a pivot
    // handover, when the carried values belong to a now-retired datum.
    // RTA-2 (network.scoped_handover_response): only the CHANGED
    // constellation(s)' datum retired — a per-constellation clock gauge is
    // independent of its siblings, so carries of unaffected constellations
    // remain valid and fold normally; the per-key skip below drops exactly
    // the retired-datum keys.  OFF reproduces the legacy all-or-nothing
    // skip byte-identically (the outer gate collapses to !datum_changed and
    // the per-key skip can never trigger inside it).
    if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on) {
        std::vector<NetworkEstimator::SharedPrior> priors;
        priors.reserve(prev_carry.size());
        for (const CarryPrior& cp : prev_carry) {
            if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
            // Fix A hold: the held phase-bias prior is injected by its own block
            // BELOW, unconditionally — the phase-ensemble datum is orthogonal to
            // the CLOCK pivot handover this clock-frame carry is gated on, so a
            // clock datum change must NOT drop it (else a held sat, whose Σz=0 fold
            // buildEnsembleGauges skipped, is left un-datumed and the epoch withholds).
            if (phase_hold_on && cp.kind == SharedParamKind::PhaseBias) continue;
            if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
            // Card 3 (eq34) D4 hygiene: a re-referenced (sat,σ) stepped its b_s
            // frame at this epoch's election — marginalize its carried PB prior
            // for ONE epoch (it belongs to the retired frame; folding it yanks
            // the new frame by up to the full re-level).  Mirrors the RTA-2
            // scoped skip; always empty when eq34 is off.
            if (cp.kind == SharedParamKind::PhaseBias
                && eq34_reref.count({cp.sat, cp.signal})) continue;
            if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
            const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                           : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                    : q_pb;
            // A clock-monitor-flagged satellite folds with the large REINIT variance (a soft
            // large-q reinitialization): the weak prior still anchors the parameter at its
            // previous value — curbing the collective datum drift a hard refloat causes — but
            // is loose enough for the data to walk a frozen clock back toward truth.
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * std::max(0.0, dt);
            // B4: floor the D2 phase-bias / code-bias carry variance (caps Λ so a stiff
            // zero-q carry cannot lock a biased product in), mirroring the ambiguity-carry
            // floor.  Only on the non-refloat path (a clk-monitor refloat is deliberately
            // loose); clock is out of scope (own two-state model / monitor).  0 = off.
            if (!cp.inflate_q) {
                const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                   : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                             : 0.0;
                if (floor > 0.0) var_eff = std::max(var_eff, floor);
            }
            if (!(var_eff > 0.0)) continue;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;
            if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                clk_prior_info[cp.sat] = p.info;   // B7: scalar clock carry info on this column
            priors.push_back(p);
        }
        est.addSharedPrior(priors);  // skips params absent from this epoch's layout
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), priors.begin(), priors.end());
    } else if ((!datum_changed || scoped_handover) && !prev_carry.empty()) {
        // C1 (network.shared_carry_full_cov): fold the carried block as ONE dense
        // information prior Λ = (P_prev + Q)⁻¹ — the information-form KF time
        // update on the shared border, recovering the cross-covariances (most
        // critically clock↔phase-bias) the diagonal stash above discards.  Entry
        // selection, per-key skip rules, and the per-key var_eff surgery
        // (inflate_q reinit, q·dt inflation, B4 floors) are IDENTICAL to the
        // diagonal branch; the only new content is the retained off-diagonals.
        // Keys with no matrix row (first fold after a warm resume, or after a
        // stash-side non-finite screen) fold through the legacy diagonal path —
        // the mode degrades per-key, it never breaks.  Keys skipped by rule are
        // MARGINALIZED (their rows/cols of P are simply not selected), which is
        // the exact treatment for a departed/retired parameter.
        std::vector<NetworkEstimator::SharedPrior> dkeys;     // dense-eligible set
        std::vector<std::size_t> drow;                        // row in prev_carry_cov.P
        std::vector<double> dvar_eff;                         // per-key var_eff (diag + fallback)
        std::vector<char> dinflate;                           // inflate_q per dense key
        std::vector<NetworkEstimator::SharedPrior> leftovers; // no matrix row → diagonal fold
        dkeys.reserve(prev_carry.size());
        drow.reserve(prev_carry.size());
        dvar_eff.reserve(prev_carry.size());
        dinflate.reserve(prev_carry.size());
        for (const CarryPrior& cp : prev_carry) {
            if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
            if (phase_hold_on && cp.kind == SharedParamKind::PhaseBias) continue;  // Fix A hold: injected separately (datum-change-independent)
            if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
            // Card 3 (eq34) D4 hygiene: marginalize re-referenced PB keys for
            // this epoch (retired frame) — see the diagonal branch above.
            if (cp.kind == SharedParamKind::PhaseBias
                && eq34_reref.count({cp.sat, cp.signal})) continue;
            if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
            const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                           : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                    : q_pb;
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * std::max(0.0, dt);
            if (!cp.inflate_q) {
                const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                   : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                             : 0.0;
                if (floor > 0.0) var_eff = std::max(var_eff, floor);
            }
            if (!(var_eff > 0.0)) continue;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
            const auto rit = prev_carry_cov.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (rit == prev_carry_cov.row.end()) {
                if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                    clk_prior_info[cp.sat] = p.info;   // B7: diagonal-carry info (no dense row)
                leftovers.push_back(p);
                continue;
            }
            dkeys.push_back(p);
            drow.push_back(rit->second);
            dvar_eff.push_back(var_eff);
            dinflate.push_back(cp.inflate_q ? 1 : 0);
        }
        const std::size_t kd = dkeys.size();
        if (kd > 0) {
            // P_eff over the dense set: subset of the stashed posterior covariance;
            // diagonal := var_eff (the stashed diagonal equals cp.var by
            // construction, so this IS the legacy q·dt inflation + B4 floor); a
            // clock-monitor-flagged key is DECORRELATED before its diagonal becomes
            // the reinit variance (its old cross-covariances are stale once the
            // variance is replaced — and a swapped diagonal under live off-diagonals
            // could make P_eff indefinite).
            Eigen::MatrixXd cov_eff(kd, kd);
            for (std::size_t a = 0; a < kd; ++a)
                for (std::size_t b = 0; b < kd; ++b)
                    cov_eff(a, b) = prev_carry_cov.P(drow[a], drow[b]);
            for (std::size_t a = 0; a < kd; ++a) {
                if (dinflate[a]) {
                    cov_eff.row(static_cast<Eigen::Index>(a)).setZero();
                    cov_eff.col(static_cast<Eigen::Index>(a)).setZero();
                }
                cov_eff(a, a) = dvar_eff[a];
            }
            try {
                Eigen::VectorXd lam;
                est.addSharedPriorDense(dkeys, cov_eff,
                                        anchor_info_trace.is_open() ? &lam : nullptr);
                if (anchor_info_trace.is_open())
                    for (std::size_t a = 0; a < kd; ++a)
                        if (dkeys[a].kind == SharedParamKind::Clock)
                            clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // B7: folded Λ(c,c)
                if (within_epoch_armed) {
                    cap_dense_keys = dkeys;      // replayed in resolve_dropping
                    cap_dense_cov  = cov_eff;
                }
            } catch (const std::exception& e) {
                // Numerical fallback: fold the SAME keys as independent diagonal
                // priors with 1/var_eff — exactly the legacy semantics for this
                // epoch.  Self-healing: the next emit re-stashes a fresh block.
                log->warn("Network (C1): dense carried-covariance fold failed ({}) — "
                          "falling back to the diagonal carry for this epoch.",
                          e.what());
                traces.emitEvent(t, "D2_DENSE", "fallback_diagonal", "", "",
                          static_cast<double>(kd), std::nullopt, "fallback",
                          e.what());   // non-PD / non-finite / conditioning budget — emitEvent sanitizes commas
                for (std::size_t a = 0; a < kd; ++a) {
                    if (anchor_info_trace.is_open() &&
                        dkeys[a].kind == SharedParamKind::Clock)
                        clk_prior_info[dkeys[a].sat] = dkeys[a].info;
                    leftovers.push_back(dkeys[a]);
                }
            }
        }
        est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), leftovers.begin(), leftovers.end());
    }

    // (Fix A hold: the held phase-bias datum is injected PRE-gate above, next to
    // the SH-VTEC prior — it is a datum, so the gate must see it.)
}

void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkRunTraces& traces, const GnssTime& t,
                                NetworkEstimator& est,
                                const std::vector<std::string>& registered_clock_sats,
                                bool clk_model_warm, bool datum_changed,
                                const std::set<char>& changed_consts,
                                double pl_now_s, double dt, bool within_epoch_armed) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& clock_osc = ctx.carry.clock_osc;
    const double clk_model_q1 = ctx.knobs.clk_model_q1;
    const double clk_model_q2 = ctx.knobs.clk_model_q2;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    auto& clk_pred = ws.clk_pred;
    auto& cap_priors = ws.cap_priors;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // RTA-2 scope predicate — re-declared verbatim over the explicit
    // (datum_changed, changed_consts) parameters; identical read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };

    // --- Two-state oscillator clock carry-forward prior (network.clock_model) ------------
    // The satellite clock's prior comes from the physical clock model: predict [offset,drift]
    // forward and fold the offset (mean = drift-following prediction, info = 1/predicted-var).
    // Because the prediction is tight (~cm), the post-solve innovation exposes a lock-in that
    // the scalar random walk would freeze.  Skipped on a pivot handover (retired datum) and
    // for the first warm epoch of each satellite (no prediction until it is seeded).
    // RTA-2 (network.scoped_handover_response): a handover retires only the changed
    // constellation(s)' datum, so under scoping their satellites alone skip the prediction
    // (they re-seed from this epoch's new-datum solve in the post-solve update); unaffected
    // constellations' oscillators predict normally.  OFF is byte-identical (outer gate
    // collapses to !datum_changed; the per-sat skip can never trigger inside it).
    if (clk_model_warm && (!datum_changed || scoped_handover)) {
        std::vector<NetworkEstimator::SharedPrior> clkp;
        clkp.reserve(clock_osc.size());
        for (const std::string& sat : registered_clock_sats) {
            if (handover_affects(sat)) continue;  // RTA-2: retired-datum sat (scoped mode only)
            auto it = clock_osc.find(sat);
            if (it == clock_osc.end() || !it->second.init) continue;
            // P0-3: predict over THIS satellite's true elapsed time, not the global
            // epoch dt.  A satellite absent for several epochs (eclipse exclusion is
            // ON in the clock-model config) would otherwise get an hours-old state
            // predicted over ONE dt of Q — a falsely-tight prior.  Past a gap bound
            // its carried state is stale: drop the prior entirely (reseed) so the
            // clock re-acquires from data + the a-priori anchor rather than trusting
            // an ancient value.  (Within the bound the Q·dt inflation over the true
            // gap naturally de-weights the prior — the honest, self-limiting behavior.)
            const double sat_dt = (it->second.t_last > 0.0)
                                      ? (pl_now_s - it->second.t_last) : std::max(0.0, dt);
            if (!(sat_dt >= 0.0) || sat_dt > clk_model_max_gap) {
                // T5 (V2 blind spot): the stale-carry reseed is otherwise SILENT.  Naturally
                // edge-triggered — the update reseeds t_last, so next epoch is fresh.
                traces.emitEvent(t, "T5_clock_model", "stale_prior_reseed", sat, "", sat_dt,
                          clk_model_max_gap, "reseed", "carry past gap");
                continue;   // R11: stale → no prior (reseed)
            }
            const ClockOscPred pr = clockOscPredict(it->second, sat_dt,
                                                    clk_model_q1, clk_model_q2);
            if (!(pr.p00 > 0.0) || !std::isfinite(pr.x0)) continue;
            clk_pred[sat] = pr;
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = sat; p.signal = "";
            p.value = pr.x0; p.info = 1.0 / pr.p00;
            if (anchor_info_trace.is_open()) clk_prior_info[sat] = p.info;  // B7: two-state clock-model prior info
            clkp.push_back(p);
        }
        est.addSharedPrior(clkp);
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), clkp.begin(), clkp.end());
    }
}

void networkHarvestLocalCarries(NetworkRunContext& ctx, const GnssConfig& config_,
                                NetworkEstimator& est, const Eigen::VectorXd& x_s,
                                std::vector<NetStation>& stations,
                                const std::vector<int>& est_index_of,
                                const std::vector<StationAssembly>& asm_res,
                                const NetworkSbasis::Plan& plan,
                                const SphericalHarmonicIonosphere* sh_model,
                                const SphericalHarmonicIonosphere* sh_model2,
                                NetworkCarryVce& carry_vce_est, bool carry_warm,
                                double pl_now_s,
                                const std::vector<std::string>& registered_clock_sats,
                                const std::set<std::string>& shared_phase_keys,
                                const std::vector<std::pair<std::string, std::string>>& registered_code_keys) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& rpb_carry = ctx.carry.rpb_carry;
    auto& eq34_reref = ctx.carry.eq34_reref;
    auto& n_cpl_harvest_fail = ctx.counters.n_cpl_harvest_fail;
    auto& n_cpl_stash_empty = ctx.counters.n_cpl_stash_empty;
    auto& n_cpl_stash = ctx.counters.n_cpl_stash;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond;
    const bool   carry_vce = ctx.knobs.carry_vce;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double pl_q_amb = ctx.knobs.pl_q_amb;
    const double pl_var_floor = ctx.knobs.pl_var_floor;
    const bool   zwd_on = ctx.knobs.zwd_on;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;

    // --- Phase-leveling carry: harvest posterior + stash (Increment 1) ----
    // For each present non-pivot station, recover its ambiguities' ABSOLUTE
    // values (x_lin + back-substituted Δx) and their conditional variances
    // (the cheap per-column diagonal harvest, NOT the full per-station inverse)
    // and refresh the persistent carry.  Then drop entries unseen beyond the
    // max gap (a stale-carry / map-growth guard — distinct from the slip-driven
    // REFLOAT/DROP of later increments).
    if (carry_warm) {
        // Card 1 (carry_conditional_coupling): build, ONCE per epoch, the shared
        // column → identity descriptor map and the datum-frame marginal-variance
        // diagonal, so each carried local's regression on the FREE shared border can
        // be stashed in a layout-stable (identity-keyed) form.  Empty / unused on the
        // legacy diagonal path → byte-identical.
        std::map<Eigen::Index, CoupledShared> col2desc;
        Eigen::MatrixXd cov_df;   // shared covariance, datum-pinned + ensemble-gauge cols zeroed
        if (carry_cpl_cond) {
            auto regcol = [&](CoupledShared d, Eigen::Index col) {
                if (col >= 0 && col < est.numShared()) col2desc.emplace(col, std::move(d));
            };
            for (const std::string& sat : registered_clock_sats) {
                try { CoupledShared d; d.kind = SharedParamKind::Clock; d.sat = sat;
                      regcol(std::move(d), est.clockColumn(sat)); } catch (...) {}
            }
            for (const auto& pk : shared_phase_keys) {
                const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                if (!parts) continue;
                try { CoupledShared d; d.kind = SharedParamKind::PhaseBias;
                      d.sat = parts->satellite; d.signal = parts->signal;
                      regcol(std::move(d),
                             est.phaseBiasColumn(parts->satellite, parts->signal)); } catch (...) {}
            }
            for (const auto& kv : registered_code_keys) {
                try { CoupledShared d; d.kind = SharedParamKind::CodeBias;
                      d.sat = kv.first; d.signal = kv.second;
                      regcol(std::move(d), est.codeBiasColumn(kv.first, kv.second)); } catch (...) {}
            }
            if (config_.estimateNetworkIonoSh() && sh_model)
                for (const ShLayer& lay :
                     shLayers(config_.getGlobalVtecShConfig(), sh_model, sh_model2))
                    for (const auto& c : lay.model->coeffs()) {
                        try { CoupledShared d; d.kind = SharedParamKind::VtecSh;
                              d.n = c.n; d.m = c.m; d.trig = c.trig;
                              d.sh_layer = lay.layer;
                              regcol(std::move(d),
                                     est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer)); } catch (...) {}
                    }
            // P_prev,jj a departed shared column is marginalized with next epoch: the
            // datum-frame marginal variance (datum-pinned + ensemble-gauge cols EXACT = 0,
            // mirroring conditionalLocalVarianceDiag's frame).
            cov_df = est.sharedCovariance();
            for (const auto& p : plan.shared_datum)
                if (p.column >= 0 && p.column < cov_df.rows()) {
                    cov_df.row(p.column).setZero(); cov_df.col(p.column).setZero();
                }
            est.zeroPhaseGaugeColumns(cov_df);
        }
        // Card 1: harvest one carried column's PURE conditional variance (N_rr⁻¹)_cc
        // + its regression on the free shared border (identity-keyed, sparsified).
        // Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
        auto harvestCoupling = [&](std::size_t est_idx, Eigen::Index col)
                -> std::pair<double, std::vector<CoupledShared>> {
            NetworkNormalEquations::StationRegression reg;
            try { reg = est.stationRegression(est_idx, col); }
            catch (...) { return {-1.0, {}}; }
            std::vector<CoupledShared> cpl;
            const Eigen::Index ns = reg.row.size();
            if (ns > 0) {
                const double maxabs = reg.row.cwiseAbs().maxCoeff();
                const double thr = kCouplingSparsifyRelThresh * maxabs;
                for (Eigen::Index j = 0; j < ns; ++j) {
                    const double mj = reg.row(j);
                    if (std::abs(mj) <= thr) continue;          // sparsify near-zero entries
                    const auto dit = col2desc.find(j);
                    if (dit == col2desc.end()) continue;        // col not in the enumerated shared set
                    // Card 1e: a datum-pinned OR ensemble-gauge column is KEPT in the row
                    // (coefficient + its ŝ_prev RHS term) with p_prev = 0 ("exactly known";
                    // the zeroed cov_df diagonal mirrors conditionalLocalVarianceDiag's
                    // frame).  The FULL regression row is exactly GAUGE-INVARIANT — every
                    // station data row satisfies A_s·g_s + A_r·g_r = 0 for each structural
                    // gauge direction, so M = e_cᵀN_rr⁻¹N_rs has Mᵀg_s = −g_r(c) and the
                    // row's +1 local completes the cancellation.  The former FREE-column
                    // restriction AMPUTATED the pinned columns' coefficients, leaving
                    // M'ᵀg ≠ 0: every coupling row then observed the clock gauge, the W5
                    // null-space gate saw "0 null directions vs declared dim" and WITHHELD
                    // every epoch that carried rows — which kept the whole coupling carry
                    // structurally inert in the field (the byte-identical-A/B trap).
                    // Under the datum the pinned term is exact (Dirichlet elimination);
                    // p_prev = 0 degrades every other path correctly: the κ time update
                    // falls through unshrunk (a pinned column does not move) and a
                    // departed pinned column marginalizes to +0.
                    const double pj = (j < cov_df.rows() && cov_df(j, j) > 0.0)
                                          ? cov_df(j, j) : 0.0;
                    CoupledShared cs = dit->second;
                    cs.coeff  = mj;
                    cs.s_prev = x_s(j);
                    cs.p_prev = pj;
                    cpl.push_back(std::move(cs));
                }
            }
            return {reg.cond_var, std::move(cpl)};
        };
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            // Prioritized: harvest the former pivot too — its non-anchored arcs
            // are in R.pl_amb; anchored (pinned) keys were excluded at build, so
            // inject and harvest partition on the SAME per-(sat,signal) predicate.
            if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
            const StationAssembly& R = asm_res[i];
            if (R.pl_amb.empty()) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_amb.size());
            for (const auto& a : R.pl_amb) cols.push_back(a.col);
            Eigen::VectorXd xr, vard;
            try {
                xr = est.reconstructStation(est_idx, x_s);
                if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc, not this MARGINAL
                    vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                const PlAmb& pa = R.pl_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, pa.col);
                    var = hc.first; coupling = std::move(hc.second);
                    // Card 1/1d observability: a failed harvest drops the carry entirely;
                    // an EMPTY regression silently degrades the row to a plain diagonal.
                    if (!(var > 0.0) || !std::isfinite(var)) ++n_cpl_harvest_fail;
                    else if (coupling.empty())               ++n_cpl_stash_empty;
                    else                                     ++n_cpl_stash;
                } else {
                    var = (a < static_cast<size_t>(vard.size())) ? vard(a) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                // M2 (LS-VCE): fold this ambiguity carry's post-fit innovation
                // (carried prior − freshly solved) into the monitor BEFORE the
                // stash overwrites the prior.  Qualify only rows that actually
                // carried a prior this epoch — mirror the inject predicate: a
                // live prior within max-gap, not a slip/refloat (pending reseed),
                // and not a Card-3 re-referenced arc (a frame step, not noise).
                if (carry_vce) {
                    const auto oc = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    if (oc != local_carry.end()
                        && !stations[i].sm->isAmbiguityPendingReseed(pa.amb_key)
                        && !eq34_reref.count({pa.sat, pa.signal})) {
                        const double gapv = pl_now_s - oc->second.last_seen_s;
                        if (gapv >= 0.0 && gapv <= pl_max_gap_s) {
                            double fold_var =
                                oc->second.var + pl_q_amb * std::max(0.0, gapv);
                            if (pl_var_floor > 0.0)
                                fold_var = std::max(fold_var, pl_var_floor);
                            carry_vce_est.addRow(
                                "pl", pa.sat, oc->second.value_abs - value_abs,
                                gapv, var, fold_var);
                        }
                    }
                }
                local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)] =
                    LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
                // One-shot IF-phase refloat: the fresh post-slip value is now
                // stashed, so drop any pending-reseed mark and let the next epoch
                // resume normal carry on the new arc.  Cleared only after a
                // SUCCESSFUL stash — a skipped non-finite value keeps the mark so
                // the stale pre-slip carry is never re-trusted.  No-op when unmarked.
                stations[i].sm->clearPendingReseed(pa.amb_key);
            }
        }
        for (auto it = local_carry.begin(); it != local_carry.end();) {
            if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = local_carry.erase(it);
            else ++it;
        }
        // --- ZWD carry (B9): harvest posterior ZWD + stash (mirrors the ambiguity
        //     harvest; independent loop so it works regardless of ambiguity presence).
        if (zwd_on) {
            for (size_t i = 0; i < stations.size(); ++i) {
                // B9: harvest the pivot's ZWD too — R.pl_zwd_present is set for the
                // pivot now that the carry inject above runs for it.
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (!R.pl_zwd_present) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(
                            est_idx, {R.pl_zwd.col}, plan.shared_datum);
                } catch (...) { continue; }
                if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
                const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, R.pl_zwd.col);
                    var = hc.first; coupling = std::move(hc.second);
                } else {
                    var = (vard.size() > 0) ? vard(0) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                zwd_carry[static_cast<int>(i)] =
                    LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
            }
            for (auto it = zwd_carry.begin(); it != zwd_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = zwd_carry.erase(it);
                else ++it;
            }
        }
        // --- Card 3 (eq34): RPB carry — harvest δ̂_{r,σ} + stash + prune ----
        // Mirrors the ZWD harvest.  DELIBERATELY no Card-1 coupling harvest
        // for RPB columns: coupling rows would hand the RPB a de-facto
        // shared-border footprint (the D6 v2 caveat zone — tuGaugeRestore
        // group exclusions); the diagonal conditional variance keeps the
        // carry strictly local and conservative.
        if (eq34_rpb_q > 0.0) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_rpb.empty()) continue;
                const std::size_t est_idx =
                    static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                std::vector<Eigen::Index> cols;
                cols.reserve(R.pl_rpb.size());
                for (const auto& t : R.pl_rpb) cols.push_back(std::get<1>(t));
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        est_idx, cols, plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_rpb.size(); ++a) {
                    const auto& [rpb_key, rcol, rx_lin] = R.pl_rpb[a];
                    if (rcol < 0 || rcol >= xr.size()) continue;
                    const double value_abs = rx_lin + xr(rcol);
                    const double var = (a < static_cast<std::size_t>(vard.size()))
                                           ? vard(a) : -1.0;
                    if (!std::isfinite(value_abs) || !(var > 0.0)
                        || !std::isfinite(var)) continue;
                    rpb_carry[{static_cast<int>(i), rpb_key}] =
                        LocalCarry{value_abs, var, pl_now_s};
                }
            }
            for (auto it = rpb_carry.begin(); it != rpb_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s)
                    it = rpb_carry.erase(it);
                else ++it;
            }
        }
    }
}

void networkRetroUraBookkeeping(NetworkRunContext& ctx,
                                const std::set<std::string>& clock_refloat_sats,
                                const std::set<std::string>& retro_flag_sats,
                                bool retro_feed) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& retro_active = ctx.carry.retro_active;
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const bool   ura_retro = ctx.knobs.ura_retro;

    // B1: after all post-emit QC tiers, record the flagged sats for the NEXT emit's
    // retro-bump (per-SATELLITE disc bump) + post-refloat URA inflation horizon.
    // The flag set is the union of clock_refloat_sats (clock-model / clock-monitor,
    // plus the coupled RAIM tiers) and retro_flag_sats (the tier-independent P0-2
    // accumulator) — so the retro-bump reaches every QC tier regardless of the
    // clock_monitor_couple_raim opt-in.
    if (retro_feed) {
        std::set<std::string> flagged_now;
        for (const std::string& sat : clock_refloat_sats)
            if (!sat.empty()) flagged_now.insert(sat);
        for (const std::string& sat : retro_flag_sats)
            if (!sat.empty()) flagged_now.insert(sat);

        if (ura_refloat_inflate > 1.0 && pl_warmup > 0)
            for (const std::string& sat : flagged_now) ura_refloat_horizon[sat] = pl_warmup;

        // Retro-bump is EDGE-triggered (P1-3): queue only a sat that just became
        // flagged (not already in retro_active), so a satellite that stays flagged
        // through several recovery epochs is reset ONCE at the consumer, not every
        // emit.  A sat that clears drops out of retro_active, so a later re-flag
        // re-arms the bump.  ura_retro_sats is consumed at the next emit.
        if (ura_retro) {
            for (const std::string& sat : flagged_now)
                if (!retro_active.count(sat)) ura_retro_sats.insert(sat);
            retro_active = std::move(flagged_now);
        }

        // Age out the post-refloat inflation horizon one epoch at a time.
        if (ura_refloat_inflate > 1.0)
            for (auto it = ura_refloat_horizon.begin(); it != ura_refloat_horizon.end();) {
                if (--it->second <= 0) it = ura_refloat_horizon.erase(it);
                else ++it;
            }
    }
}

void networkStashCarry(NetworkRunContext& ctx, const GnssConfig& config_,
                       NetworkEstimator& est, const Eigen::VectorXd& x_s,
                       const NetworkSbasis::Plan& plan,
                       const SphericalHarmonicIonosphere* sh_model,
                       const SphericalHarmonicIonosphere* sh_model2,
                       const std::set<std::string>& clock_refloat_sats,
                       const std::vector<std::string>& registered_clock_sats,
                       const std::set<std::string>& shared_phase_keys,
                       const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                       const std::shared_ptr<spdlog::logger>& log) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    auto& prev_clock_refloat_sats = ctx.carry.prev_clock_refloat_sats;
    auto& code_bias_prev_ = ctx.carry.code_bias_prev_;
    auto& code_bias_ifpin_pair_ = ctx.carry.code_bias_ifpin_pair_;
    auto& code_bias_sat_seen_ = ctx.carry.code_bias_sat_seen_;
    auto& this_epoch_cb_ifpin = ctx.carry.this_epoch_cb_ifpin;
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;
    const bool   carry_cpl_tu = ctx.knobs.carry_cpl_tu;
    const bool   clk_model_on = ctx.knobs.clk_model_on;
    const bool   carry_invariant = ctx.knobs.carry_invariant;
    const bool   phase_hold_on = ctx.knobs.phase_hold_on;
    const double phase_hold_var = ctx.knobs.phase_hold_var;
    const bool   cb_time_linked = ctx.knobs.cb_time_linked;
    const bool   fullcov_on = ctx.knobs.fullcov_on;

    // --- D2 stash for the next epoch ------------------------------------
    prev_carry.clear();
    prev_carry_cov.clear();   // C1: rebuilt below when the dense carry is on
    std::vector<Eigen::Index> fc_cols;  // C1: shared-border column per dense row
    // Fix A (covariance-carry consistency — the load-bearing companion to the
    // point-solve fold).  Under the anchor-free ensemble phase-bias datum the
    // gauge is realized POST-reduce by the rank-1 correction Ñ_ss += g gᵀ/β
    // (@ref NetworkNormalEquations::foldReducedSumZeroGauges), NOT by a local
    // pre-reduce pin as in the anchor mode.  A satellite phase-bias column is the
    // gauge (null) direction of the UNfolded reduced normals — the fold makes the
    // system invertible but leaves that column's slot of the reduced covariance
    // carrying the fold's ARTIFICIAL gauge-direction variance (an over-tight,
    // epoch-by-epoch self-reinforcing value set by the sum-to-zero reference, not
    // a physical uncertainty — the ensemble reference is re-realized EXACTLY every
    // epoch) plus its phase↔clock / phase↔code cross terms.  Feeding that column
    // into the D2 carry propagates the fold into next epoch's prior: the stiff,
    // gauge-referenced phase-bias prior biases the coupled satellite clock through
    // N_ss[clk,pb], clock_model amplifies it, and the pair runs away exponentially
    // from epoch 2 (measured ~7×/epoch → 5e8 m).  Cure: treat every ensemble
    // phase-bias gauge column as an EXACT datum (variance 0) in the covariance the
    // carry harvests — the exact analogue of how conditionalLocalCovariance /
    // conditionalLocalVarianceDiag zero the datum-PINNED shared columns before
    // reconstructing a conditional covariance ("datum-pinned rows/cols zeroed →
    // treat as exact").  Zeroing the ROW AND COLUMN drops both the over-tight
    // diagonal (→ the diagonal stash's var>0 guard skips the key) and the
    // phase↔clock/code cross-covariance (→ the key is excluded from the C1 dense
    // block too), while the gauge-INVARIANT clock / code-bias variances (which
    // the S-transform leaves identical to the anchor datum) pass through
    // untouched.  Continuity of the emitted phase bias is preserved by the LOCAL
    // ambiguity carry (phase_leveling) + the stable ensemble constraint, not by
    // this shared carry (which under the fold merely double-counted that info and
    // added the contamination).  Anchor / OFF mode: cov aliases
    // est.sharedCovariance() unchanged → byte-identical.  NB: zeroing the g
    // DIRECTION (g = λ·N_ss·e_pb, which has clock components) would instead corrupt
    // the correct clock covariance — the contaminated direction is the phase-bias
    // COLUMN e_pb, not g.
    const Eigen::MatrixXd& cov_src = est.sharedCovariance();
    Eigen::MatrixXd cov_ens;   // ensemble-only: the gauge-exact copy
    const bool ens_phase_gauge = config_.networkPhaseDatumEnsemble();
    if (ens_phase_gauge) {
        cov_ens = cov_src;
        est.zeroPhaseGaugeColumns(cov_ens);   // treat each folded gauge column as exact
    }
    const Eigen::MatrixXd& cov = ens_phase_gauge ? cov_ens : cov_src;
    std::set<Eigen::Index> pinned_cols;
    for (const auto& p : plan.shared_datum) pinned_cols.insert(p.column);
    auto stash = [&](SharedParamKind kind, const std::string& sat,
                     const std::string& signal, Eigen::Index col, bool inflate) {
        // Skip the datum-pinned columns.  Their solve covariance is 1/pin_scale
        // (not ~0), but the explicit datum re-pins them every epoch (its
        // Dirichlet elimination overrides any prior on them), and carrying one
        // forward would wrongly anchor a retired pivot to its old datum value
        // after a handover.
        if (pinned_cols.count(col)) return;
        const double var = cov(col, col);
        if (!(var > 0.0)) return;
        CarryPrior cp;
        cp.kind = kind; cp.sat = sat; cp.signal = signal;
        cp.value = x_s(col); cp.var = var; cp.inflate_q = inflate;
        prev_carry.push_back(cp);
        if (fullcov_on) {   // C1: register this entry's dense row (same skip rules)
            prev_carry_cov.row.emplace(sharedCarryKey(kind, sat, signal),
                                       fc_cols.size());
            fc_cols.push_back(col);
        }
    };
    // A satellite flagged by the clock monitor (or, opt-in, the within-epoch / post-fit RAIM)
    // has its carried SHARED state REINITIALIZED rather than frozen.  Two modes: with
    // clk_mon_reinit_sig > 0 (default) it is STILL stashed but marked inflate_q, so next epoch
    // folds it with the loose reinit variance (a weak anchor that lets a frozen clock walk to
    // truth while curbing the datum drift a hard refloat causes); with clk_mon_reinit_sig ≤ 0
    // it is WITHHELD entirely (hard skip-stash refloat — no prior).  clock_refloat_sats is
    // empty unless network.clock_monitor, so the stash is byte-identical when the monitor is
    // off (nothing flagged → inflate=false → the ordinary stiff-carry stash).
    const bool full_refloat = !(clk_mon_reinit_sig > 0.0);   // reinit_sigma ≤ 0 → hard skip-stash
    // Card 1d: carry this stash epoch's clock-monitor flags across to the next
    // inject (hard-refloat mode skip-stashes the flagged sat entirely, so the
    // flag cannot ride on prev_carry.inflate_q).  Gated to keep OFF runs pristine.
    if (carry_cpl_tu) prev_clock_refloat_sats = clock_refloat_sats;
    // The satellite clock is stashed to prev_carry ONLY in scalar-carry mode.  Under
    // network.clock_model the clock is carried by clock_osc (updated above), so it is not
    // stashed here — the two-state model is its sole cross-epoch memory.
    if (!clk_model_on)
        for (const std::string& sat : registered_clock_sats) {
            const bool flagged = clock_refloat_sats.count(sat) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
        }
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Card 4: the DD invariant carry is the phase time-link — do NOT also stash the
        // S-dependent shared PhaseBias carry (it would double-count in next epoch's D2 /
        // hold / dense fold).  Clock / code / SH / ZWD carries are unaffected.
        if (carry_invariant) continue;
        const bool flagged = clock_refloat_sats.count(parts->satellite) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col =
            est.phaseBiasColumn(parts->satellite, parts->signal);
        if (phase_hold_on) {
            // Seed-once + time-lock: carry b_s toward its realized value with the
            // fixed (clean) hold-variance — the temporal datum that stops the
            // ~metre/epoch re-realization.  Deliberately NOT cov(col,col): the
            // ensemble fold leaves that column carrying an artificial gauge
            // variance (carrying it is the epoch-2 fast runaway this whole fix
            // family guards).  A stiff, hand-set variance makes the carried value
            // the datum reference next epoch (buildEnsembleGauges then skips the
            // Σz=0 fold for it).  Datum-pinned columns (none for PB) and non-finite
            // values are still excluded.
            if (pinned_cols.count(col)) continue;
            const double v = x_s(col);
            if (!std::isfinite(v)) continue;
            CarryPrior cp;
            cp.kind = SharedParamKind::PhaseBias;
            cp.sat = parts->satellite; cp.signal = parts->signal;
            // inflate_q=false always: a clock-monitor SOFT refloat of this sat
            // (the loose-reinit path the legacy stash honors) deliberately does NOT
            // loosen the phase-bias hold — the phase-ensemble datum is orthogonal to
            // a clock fault, so the bias stays time-locked.  (A HARD refloat is still
            // handled: `flagged && full_refloat` skip-stashes above, so the sat
            // re-seeds next epoch.)
            cp.value = v; cp.var = phase_hold_var; cp.inflate_q = false;
            prev_carry.push_back(cp);
        } else {
            stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal,
                  col, flagged);
        }
    }
    // Card 2: refresh the time-linked previous-value map from THIS kept epoch's
    // solved code biases.  Cleared + rebuilt (a departed satellite drops out → its
    // return re-seeds IT alone) in lock-step with the D2 carry — same disposition
    // rules (a hard-refloated sat is skipped from BOTH, so it re-seeds next epoch,
    // its re-reference disc-signalled).  NOT snapshotted.  Empty/untouched when off.
    if (cb_time_linked) {
        code_bias_prev_.clear();
        // Advance the IF-pin comparison baseline to THIS kept epoch's pairs (a pair
        // recorded on a since-withheld epoch never reaches here, so the baseline
        // tracks emitted/kept datum realizations only).
        code_bias_ifpin_pair_ = this_epoch_cb_ifpin;
    }
    for (const auto& kv : registered_code_keys) {  // W6 satellite code bias (OSB)
        const bool flagged = clock_refloat_sats.count(kv.first) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col = est.codeBiasColumn(kv.first, kv.second);
        // SPEC_day129_frame_guards Phase 2.1: a network-wide common-mode clock
        // excursion this epoch marks the affected satellite's CODE-BIAS carry for
        // reinit — and ONLY the code-bias carry.  The clock and phase-bias carries
        // are deliberately untouched: the clock is soft (q = 0.3 m²/ep, it recovers
        // on its own in 2–4 epochs) and the phase biases measurably did not move
        // (≤ 4 mm at the DOY-129 event).  The near-static code bias is the one
        // state that ratchets, so it is the one state the guard resets.  The set is
        // empty unless network.cb_excursion_reinit ⇒ byte-identical when OFF.
        const bool cb_excursion = ctx.carry.cb_excursion_sats.count(kv.first) > 0;
        stash(SharedParamKind::CodeBias, kv.first, kv.second, col,
              flagged || cb_excursion);
        if (cb_time_linked) {
            const double v = x_s(col);
            if (std::isfinite(v)) {
                code_bias_prev_[NetworkSbasis::constellationOf(kv.first) + "|" + kv.second]
                               [kv.first] = v;
                code_bias_sat_seen_.insert(kv.first);
            }
        }
    }
    // C1: stash the dense covariance block over the clock/phase/code entries
    // just recorded (the SH block below keeps its own diagonal stash — its
    // carry is the separate pre-gate fold).  One O(k²) copy from the solve's
    // posterior.  A single non-finite entry clears the whole block — the next
    // fold then degrades per-key to the diagonal path instead of poisoning
    // the next epoch's normals (the exact crash mode the SH guard below
    // screens per-entry).
    if (fullcov_on && !fc_cols.empty()) {
        const Eigen::Index kfc = static_cast<Eigen::Index>(fc_cols.size());
        prev_carry_cov.P.resize(kfc, kfc);
        for (Eigen::Index a = 0; a < kfc; ++a)
            for (Eigen::Index b = 0; b < kfc; ++b)
                prev_carry_cov.P(a, b) = cov(fc_cols[static_cast<std::size_t>(a)],
                                             fc_cols[static_cast<std::size_t>(b)]);
        if (!prev_carry_cov.P.allFinite()) {
            log->warn("Network (C1): non-finite entries in the stashed shared-"
                      "covariance block ({} keys) — dense carry cleared; the next "
                      "epoch folds the diagonal carry.", fc_cols.size());
            prev_carry_cov.clear();
        }
    }
    if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coefficients
        for (const ShLayer& lay :
             shLayers(config_.getGlobalVtecShConfig(), sh_model, sh_model2)) {
            for (const auto& c : lay.model->coeffs()) {
                const Eigen::Index col =
                    est.vtecCoeffColumn(c.n, c.m, c.trig, lay.layer);
                if (pinned_cols.count(col)) continue;   // SH cols are never datum-pinned (safety)
                const double var = cov(col, col);
                const double val = x_s(col);
                // Never carry a non-finite value/variance forward — it would poison
                // the next epoch's normals and hard-crash the solve.  The mean-
                // reverting climatology prior re-seeds the coefficient cleanly.
                if (!(var > 0.0) || !std::isfinite(val) || !std::isfinite(var)) continue;
                CarryPrior cp;
                cp.kind = SharedParamKind::VtecSh;
                cp.sh_n = c.n; cp.sh_m = c.m; cp.sh_trig = c.trig;
                cp.sh_layer = lay.layer;
                cp.value = val; cp.var = var;
                prev_carry.push_back(cp);
            }
        }
    }
}

} // namespace GnssProcessor
