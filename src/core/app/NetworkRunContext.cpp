/**
 * @file NetworkRunContext.cpp
 * @brief @internal Knob snapshot + the relocated RTB-7 fingerprint /
 *        warm-resume restore / snapshot write and the carry-lifecycle
 *        functions of Application::runNetwork.
 *
 * Decomposition INC-4 (2026-07-16) — bodies relocated VERBATIM from
 * ApplicationNetwork.cpp; each function opens with an alias block binding the
 * former local names into the context so the moved code is textually
 * unchanged.
 */

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // parseFuseClassMask / fuseClassMaskName (SPEC_prefit_census_observability INC-2)

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkClockAnchor.hpp"      // Card Q3 guarded temporal clock anchor
#include "filter/NetworkPriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkInvariantCarryDriver.hpp"   // RTB-7(c) branch-faithful carry (de)serialize
#include "products/NetworkClockEmitRebase.hpp"       // §18.10 resume frame restore: CLK-R held frame travels
#include "products/NetworkEmitRebase.hpp"            // RS-5 §21.10: the eq34 emit re-base frame travels
#include "products/NetworkPbEmitFilter.hpp"          // RS-5 §21.10: the PB emit filter state travels
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkPivotHandover.hpp"   // datumChangeAffectsKey (handover scope predicate)
#include "filter/NetworkSbasis.hpp"          // kWindUpYawModelId (wind-up model revision token)
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
    k.pl_los_scale = config_.networkPhaseLevelingLosScale();
    k.pl_local_block = config_.networkPhaseLevelingLocalBlock();
    k.pl_blk_min_age_ep = config_.networkPhaseLevelingLocalBlockMinAgeEp();   // F3 block admission age gate
    k.pl_blk_young_free = config_.networkPhaseLevelingLocalBlockYoungMode() == "free";
    // AGE-AWARE young-arc inflate (SPEC_amb_iono_split_overcount §6.17–§6.18).  The
    // loader has already disabled it (V0 -> 0) off the leveling vehicle, so the live
    // predicate is Knobs::on() alone.
    k.yng.var_m2     = config_.networkPhaseLevelingYoungInflateVarM2();
    k.yng.tau_ep     = config_.networkPhaseLevelingYoungInflateTauEp();
    k.yng.max_age_ep = config_.networkPhaseLevelingYoungInflateMaxAgeEp();
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
    k.carry_cpl_gauge  = config_.networkCarryCouplingGaugeProject();
    k.carry_cpl_reinit = config_.networkCarryCouplingReinitSources();
    k.zwd_on = config_.networkCarryZwd();
    k.zwd_in_block = config_.networkCarryZwdInBlock();  // B9-in-block; loader guarantees zwd_on + pl_local_block
    k.zwd_q = config_.networkZwdCarryQ();
    k.zwd_seam_on = config_.networkZwdSeamReseed();
    k.zwd_seam_epochs = config_.networkZwdSeamReseedEpochs();
    k.amb_seam_reinflate_var = config_.networkAmbSeamReinflateVarM2();
    k.resume_frame_restore   = config_.networkResumeFrameRestore();      // §18.10
    k.resume_coupling_restore = config_.networkResumeCouplingRestore();  // §18.12
    k.resume_datum_restore   = config_.networkResumeDatumRestore();      // RS-3 §21.7
    k.resume_emit_restore    = config_.networkResumeEmitRestore();       // RS-5 §21.10
    k.resume_station_restore = config_.networkResumeStationRestore();    // RS-6 §21.13
    k.resume_station_rxcb    = config_.networkResumeStationRestoreRxCb();
    k.resume_station_shdev   = config_.networkResumeStationRestoreShDev();
    k.resume_border_restore  = config_.networkResumeBorderRestore();     // RS-7 §21.14
    k.resume_pb_seam_mark    = config_.networkResumePbSeamMark();       // RS-4 §21.9
    k.resume_slip_warmup_ep  = config_.networkResumeSlipWarmupEp();      // §18.10
    k.resume_d5_warmup_ep    = config_.networkResumeD5WarmupEp();        // RS-2b §21.6
    k.readmit_seed_horizon_s = config_.networkReadmitSeedHorizonS();     // E2 seed-first re-admission
    k.readmit_marg_ep        = config_.networkReadmitCouplingMargEp();   // block/row re-coupling edge
    k.readmit_seed_cb        = config_.networkReadmitSeedCodeBias();     // seed the code biases too (or clock only)
    // Unconditional Z-row capture (SPEC_synthetic_minimum_network §17 finding 3):
    // any snapshot-armed run records + harvests the per-station ZWD posterior so
    // EVERY snapshot carries Z rows — read-only w.r.t. the solve, no inject.
    k.zwd_capture = !nc.state_snapshot_path.empty();
    k.iono_on = config_.networkCarryIono();  // loader guarantees phase_leveling + relevel_free_epoch (scope=relevel) + the SH/mech-B/carry_invariant mutexes
    // I-in-block; loader guarantees phase_leveling + pl_local_block + carry_cpl_cond,
    // !carry_invariant, and hard-errors on a network.carry_iono co-arm.
    k.iono_blk = config_.networkCarryIonoInBlock();
    k.iono_blk_q_m2 = config_.networkCarryIonoInBlockQM2();
    k.iono_blk_max_gap_s = config_.networkCarryIonoInBlockMaxGapS();
    // RX-CLK-in-block; loader guarantees phase_leveling + pl_local_block +
    // carry_cpl_cond, !carry_invariant, and a non-negative variance ladder.
    k.rxclk_blk        = config_.networkCarryRxClockInBlock();
    k.rxclk_q_m2       = config_.networkCarryRxClockQM2();
    k.rxclk_max_gap_s  = config_.networkCarryRxClockMaxGapS();
    k.rxclk_rate_var0  = config_.networkCarryRxClockRateVar0();
    k.rxclk_rate_q     = config_.networkCarryRxClockRateQ();
    k.rxclk_jump_k     = config_.networkCarryRxClockJumpK();
    k.rxclk_isb        = config_.networkCarryRxClockIsb();
    k.iono_scope_always = (config_.networkIonoCarryScope() == "always");
    k.iono_source_smoothed = (config_.networkIonoCarrySource() == "smoothed_gf");
    k.iono_q = config_.networkIonoCarryQ();
    k.iono_var_floor = config_.networkIonoCarryVarFloor();
    k.iono_max_gap_s = config_.networkIonoCarryMaxGapS();
    k.rx_cb_on = config_.networkCarryRxCodeBias();   // loader guarantees code_datum_time_linked
    k.rx_cb_datum = config_.networkRxCodeBiasDatum();
    k.rx_cb_datum_sigma = config_.networkRxCodeBiasDatumSigmaM();
    k.rx_cb_q = config_.networkRxCbCarryQ();
    k.rx_cb_demean = config_.networkRxCbCarryDemean();   // loader guarantees rx_cb_on
    k.rx_cb_demean_min_sta = config_.networkRxCbCarryDemeanMinStations();
    k.rx_cb_demean_within = config_.networkRxCbCarryDemeanWithin();  // loader guarantees rx_cb_demean
    k.rx_cb_step_gate = config_.networkRxCbCarryStepGate();   // loader guarantees rx_cb_on
    k.rx_cb_step_k = config_.networkRxCbCarryStepKSigma();
    k.rx_cb_harvest_demean = config_.networkRxCbHarvestDemean();   // loader guarantees rx_cb_on, forces LV-2/LV-5 off
    k.rx_cb_harvest_screen_z = config_.networkRxCbHarvestScreenZ();
    k.lv1_seed_frame = config_.networkIonoSeedFieldFrame();  // loader guarantees estimate_iono_sh
    k.lv1_min_sats = config_.networkIonoSeedFieldFrameMinSats();
    k.carry_vce = config_.networkCarryVce();
    k.vce_apply_on = config_.networkCarryVceApply();   // M2 v2; loader guarantees carry_vce
    k.ura_on = config_.networkEmitUra();
    k.ura_alpha = config_.networkUraEwmaAlpha();
    k.ura_retro = config_.networkUraRetroBump();
    k.mark_refsat_reset_disc = config_.networkMarkRefsatResetDisc();
    k.ura_refloat_inflate = config_.networkUraRefloatInflate();
    k.raim_on = config_.networkRaim();
    k.raim_k = config_.networkRaimKSigma();
    k.raim_s0_floor = config_.networkRaimSigma0Floor();
    k.raim_min_sta = config_.networkRaimMinStations();
    k.raim_arc_persist = config_.networkRaimArcPersistEpochs();
    k.raim_maturity_gate = config_.networkRaimRefloatMaturityGate();
    k.raim_maturity_epochs = config_.networkRaimRefloatMaturityEpochs();
    k.raim_withhold = config_.networkRaimWithholdSigma0();
    k.wh_escalate = config_.networkRaimWithholdAdaptEscalate();
    k.wh_free_erase = config_.networkRaimWithholdFreeErase();
    k.raim_redun = config_.networkRaimRedundancyNormalize();
    k.raim_redun_trace = config_.getNetworkConfig().trace_raim_redundancy;
    k.staf_on = config_.networkRaimStationFactor();
    k.staf_tau_s = config_.networkRaimStationFactorTauS();
    k.staf_ratio = config_.networkRaimStationFactorRatio();
    k.rate_hygiene = config_.networkRaimRateHygiene();
    k.lvlauth_on = config_.networkLevelAuthorityCsgf();
    k.lvlauth_min_n = config_.networkLevelAuthorityMinN();
    k.lvlauth_min_los = config_.networkLevelAuthorityMinLos();
    k.lvlauth_los_dump = config_.networkLevelAuthorityLosDump();
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
    k.handover_stransform = config_.networkHandoverStransform();
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
    // Card CLK-R: clock emission re-base knobs.
    // The engine itself lives in NetworkCarryState (its per-sat baselines and
    // frame accumulators must survive the epoch loop) and is configured by the
    // driver next to the PB-filter setup.
    k.clk_frame_hold = config_.networkEmitClockFrameHold();
    k.clk_jump_test  = config_.networkClockDiscJumpTest();
    // The guarded temporal clock
    // anchor.  `cta_on` is resolved against its clock_model interlock HERE, so
    // every downstream site reads one predicate: under the two-state oscillator
    // the clock is never stashed to the D2 carry, so the anchor has nothing to
    // fold and must not claim otherwise (the loader errors on the combination).
    k.clk_model_on = config_.networkClockModel();
    k.cta_on             = config_.networkClockTemporalAnchor() && !k.clk_model_on;
    k.cta_q              = config_.networkClockAnchorQ();
    k.cta_var_floor      = config_.networkClockAnchorVarFloorM2();
    k.cta_max_gap_s      = config_.networkClockAnchorMaxGapS();
    k.cta_k              = config_.networkClockAnchorK();
    k.cta_abs_m          = config_.networkClockAnchorAbsM();
    k.cta_sigma_floor_m  = config_.networkClockAnchorSigmaFloorM();
    k.cta_recouple_ep    = config_.networkClockAnchorRecoupleEpochs();
    k.cta_mark           = config_.networkClockAnchorMark();
    k.cta_resolve        = config_.networkClockAnchorResolve();
    k.cta_gauge_free     = config_.networkClockAnchorGaugeFree();
    // Card C-GF: ONE resolved predicate that every fold, withdraw and replay site reads.
    //
    // The gauge-free downdate belongs to the CARRY FOLD, not to the Q3 clock anchor —
    // it removes the datum's own direction from the carried information, which is a
    // property of carrying information across a re-realizable datum, not of anchoring a
    // clock temporally.  It was reachable only through `clock_anchor_gauge_free`, so a
    // config running the dense carry without the Q3 anchor could not have it at all.
    //
    // It MUST be a single resolved field rather than a predicate re-derived per site.
    // Fold and withdraw have to agree exactly: the withdraw subtracts what the fold
    // added, so a fold that downdated and a withdraw that did not leaves a residue
    // −ΛU(UᵀΛU)⁻¹UᵀΛ, which is NEGATIVE SEMIDEFINITE — the reduced normals stop being
    // PSD along the gauge and the constrained solve either throws on its pivot guard
    // (persistent epoch skip) or factorizes into a saddle and returns a silently wrong
    // x_s.  Resolving once here makes that disagreement unrepresentable.
    k.carry_gauge_free   = (k.cta_on && k.cta_gauge_free)
                        || (config_.networkSharedCarryFullCov()
                            && config_.networkSharedCarryGaugeFree());
    // Closure is part of the GENERALIZATION, not of the re-arming: the Q3 path keeps its
    // original rule so its arms stay byte-identical and an A/B against them isolates the
    // closure test as the single new variable.
    k.carry_gauge_free_closure = config_.networkSharedCarryFullCov()
                              && config_.networkSharedCarryGaugeFree();
    // Card Q3 §2.1 — resolve the guard/census DENOMINATOR here, once, for the same
    // reason `cta_on` resolves its interlock here: a mapping re-derived at two call
    // sites is a mapping that will eventually disagree between them, and the guard
    // and the census MUST standardize identically or the census stops being a
    // cross-check on the guard.  `auto` follows the fold, which is what every arm
    // run before this knob existed did — so `auto` is byte-identical by construction.
    {
        const auto stat = config_.networkClockAnchorGaugeFreeStat();
        k.cta_stat_proj_post  = (stat == NetworkClockAnchorStat::Post)
                             || (stat == NetworkClockAnchorStat::Proj)
                             // Card C-GF: `auto` means "follow the FOLD", and the fold is now
                             // `carry_gauge_free`.  OR-ed with the original term rather than
                             // replacing it, so a config with cta_gauge_free set but cta_on
                             // false keeps its existing denominator bit-for-bit (the census
                             // reads these same predicates) while the carry-side arming is
                             // still tracked.
                             || (stat == NetworkClockAnchorStat::Auto
                                 && (k.cta_gauge_free || k.carry_gauge_free));
        k.cta_stat_proj_prior = (stat == NetworkClockAnchorStat::Proj);
    }
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
    k.carry_invariant_pd_safe = k.carry_invariant && config_.networkCarryInvariantPdSafe();
    k.carry_aware_covariance = k.carry_invariant && config_.networkCarryAwareCovariance();
    // POOL RE-HOST.  Resolved HERE, beside the other
    // carry_invariant-derived knobs rather than up with the raim_* family, so that ONE
    // bool answers "is the re-hosted census live" for all three consumers — the driver's
    // call-site gate, the assembly's census fill and the seam's pool selector.  They must
    // not be able to disagree about whether qc_amb exists.
    //
    // The k.carry_invariant conjunct also drags in phase_datum_eq34 (see just above).
    // That is defensive redundancy, not a live discriminator: carry_invariant WITHOUT
    // eq34 is a hard load error (the loader's "network.carry_invariant requires
    // network.phase_datum_eq34" check in GnssConfigLoadNetwork.cpp returns false), so
    // no config that could exercise the difference ever reaches here.  Kept because it
    // matches how carry_invariant_pd_safe / carry_aware_covariance resolve, and because
    // if that requirement is ever relaxed this line is already correct.
    k.raim_ci_on = k.raim_on && k.carry_invariant && config_.networkRaimCarryInvariant();
    k.raim_ci_refloat = k.raim_ci_on && config_.networkRaimCarryInvariantRefloat();
    k.eq34_rpb_prior_sigma = config_.networkPhaseDatumEq34RpbPriorSigmaM();
    k.eq34_rpb_prior_w = (k.phase_datum_eq34 && k.eq34_rpb_prior_sigma > 0.0) ? 1.0 / (k.eq34_rpb_prior_sigma * k.eq34_rpb_prior_sigma) : 0.0;
    k.eq34_rpb_q = (k.phase_datum_eq34 && !k.carry_invariant) ? config_.networkPhaseDatumEq34RpbQ() : 0.0;
    k.eq34_shift_on = k.phase_datum_eq34 && config_.networkPhaseDatumEq34CarryShift() && !k.carry_invariant;
    k.eq34_longevity = k.phase_datum_eq34 && config_.networkPhaseDatumEq34LongevityElection();
    k.eq34_extend_only = k.phase_datum_eq34 && config_.networkPhaseDatumEq34ExtendOnly();
    k.eq34_slip_aware = k.phase_datum_eq34 && config_.networkPhaseDatumEq34SlipAwareElection();
    k.eq34_seed_round = k.phase_datum_eq34 && config_.networkPhaseDatumEq34SeedRound();
    k.eq34_seed_round_rehost = k.eq34_seed_round
                            && config_.networkPhaseDatumEq34SeedRoundRehostFrame();
    k.phase_datum_prioritized = config_.networkPhaseDatumPrioritized() || k.phase_datum_ensemble || k.phase_datum_eq34;
    // §9.7.10: resolved once for the run — the datum-station slip path exists only
    // under the LEGACY pivot datum (every no-pivot datum runs the slip loops from
    // i_lo = 0 already, so station 0 has ordinary coverage there).
    k.datum_station_detect = config_.networkCycleSlipDatumStationDetect()
                             && !k.phase_datum_prioritized;
    k.phase_hold_sigma = k.phase_datum_ensemble ? config_.networkPhaseDatumHoldSigmaM() : 0.0;
    k.phase_hold_on = k.phase_hold_sigma > 0.0;
    k.phase_hold_var = k.phase_hold_sigma * k.phase_hold_sigma;
    // eq34 reattach hygiene.  The loader has
    // already disarmed every increment outside an anchored election; the
    // `&& !ensemble` here is belt-and-braces so a hand-built NetworkConfig (tests)
    // cannot arm the seam on a datum that elects no anchor.
    {
        const bool anchored = k.phase_datum_prioritized && !k.phase_datum_ensemble;
        k.eq34_min_arc_ep    = anchored ? std::max(0, nc.eq34_anchor_min_arc_ep) : 0;
        k.prior_realization_max_gap_s = nc.eq34_prior_realization_max_gap_s;
        k.pin_bridge_on      = anchored && nc.phase_datum_time_linked
                               && nc.phase_datum_bridge_sigma_m > 0.0
                               && nc.phase_datum_bridge_max_ep >= 1;
        k.pin_bridge_sigma   = k.pin_bridge_on ? nc.phase_datum_bridge_sigma_m : 0.0;
        k.pin_bridge_info    = k.pin_bridge_on
                               ? 1.0 / (k.pin_bridge_sigma * k.pin_bridge_sigma) : 0.0;
        k.pin_bridge_max_ep  = k.pin_bridge_on ? nc.phase_datum_bridge_max_ep : 0;
        const bool ramp_on   = anchored && nc.eq34_fresh_pin_sigma_scale > 1.0
                               && nc.eq34_fresh_pin_ramp_ep > 0;
        k.fresh_pin_scale    = ramp_on ? nc.eq34_fresh_pin_sigma_scale : 1.0;
        k.fresh_pin_ramp_ep  = ramp_on ? nc.eq34_fresh_pin_ramp_ep : 0;
        k.anchor_hygiene_on  = k.eq34_min_arc_ep > 0 || k.pin_bridge_on || ramp_on;
        // TWO-EDGE CONTINUOUS HANDOVER.
        // Resolved here, inside `anchored`, because it is meaningful only where an
        // anchor PIN exists to hand over.  `!phase_datum_eq34` narrows it further:
        // the chain datum owns its own pin-target policy (pin-round / seed-round)
        // and the two would fight for the same target.  The loader already disarms
        // all three cases; this is the same belt-and-braces the block above uses so
        // a hand-built NetworkConfig in a test cannot arm it on the wrong election.
        k.pd_handover_continuous = anchored && !k.phase_datum_eq34
                                   && config_.networkPhaseDatumHandoverContinuous();
        k.pd_handover_max_var = k.pd_handover_continuous
                                ? config_.networkPhaseDatumHandoverMaxVarM2() : 0.0;
        k.pd_handover_max_var_ratio = k.pd_handover_continuous
                                ? config_.networkPhaseDatumHandoverMaxVarRatio() : 0.0;
        // QUEUE item 1(d): the PHASE_ANCHOR carry re-reference — same placement
        // and the same belt-and-braces as the handover knob directly above (it
        // is meaningful only where an anchor pin exists to hand over, and the
        // eq34 chain owns its own eq34_reref carry hygiene).
        k.pa_carry_reref = anchored && !k.phase_datum_eq34
                           && config_.networkPhaseAnchorCarryReref();
        // COLD ANCHOR-FALLBACK (SPEC_event_harness §15).  Resolved against
        // pd_handover_continuous, not against `anchored`, because the pass screens
        // candidates with the LEDGER's own usability predicate — with no ledger it
        // has nothing to read and would elect by coverage rank exactly as today.
        // The loader warns about the same INERT case; this is the belt-and-braces a
        // hand-built NetworkConfig (tests) needs.
        k.pd_candidacy_usable = k.pd_handover_continuous
                                && config_.networkPhaseDatumCandidacyUsable();
        k.pd_candidacy_min_age_ep = k.pd_candidacy_usable
                                ? config_.networkPhaseDatumCandidacyMinAgeEp() : 0;
        // Iteration 2, the SATELLITE clause — resolved with its parent so every site
        // reads ONE bool and an iteration-1 A/B is a single knob.
        k.pd_candidacy_satellite = k.pd_candidacy_usable
                                && config_.networkPhaseDatumCandidacySatellite();
    }
    // SBC S-basis completion (network.sbasis_complete).  Resolved here so the
    // pb_level_prev harvest (which the completion's continuity targets re-use —
    // the shared PhaseBias carry is not in prev_carry under carry_invariant) and
    // the solve-glue block read one predicate.
    k.sbasis_complete_on  = config_.networkSbasisComplete();
    k.sbasis_complete_max = config_.networkSbasisCompleteMax();
    // Flicker/gap signal integrity.  The loader
    // already disarmed every action when the census master is off; resolving them
    // here as well is belt-and-braces so a hand-built NetworkConfig (tests) cannot
    // arm a demotion behind a classifier that never runs.  `sig_withhold` carries
    // BOTH spellings of the withhold disposition (the explicit bool and the spec's
    // "scale <= 0"), so no site downstream has to know there were two.
    {
        const auto& si = nc.signal_integrity;
        k.sig_integrity_on    = si.census;
        k.sig_withhold        = si.census && (si.withhold_instead
                                              || si.unvetted_sigma_scale <= 0.0);
        k.sig_unvetted_scale  = (si.census && !k.sig_withhold
                                 && si.unvetted_sigma_scale > 1.0)
                                ? si.unvetted_sigma_scale : 1.0;
        k.sig_revet_ep        = si.census ? std::max(0, si.revet_ep) : 0;
        k.sig_flicker_max_ep  = si.census ? std::max(0, si.flicker_max_ep) : 0;
        k.sig_refloat_regain  = si.census && si.refloat_on_regain;
        // The resume warmup only MEANS anything if there is a demotion to apply.
        k.sig_resume_warmup   = si.census && si.resume_warmup_vet
                                && (k.sig_withhold || k.sig_unvetted_scale > 1.0);
    }
    // Iono-deweight RELEASE hygiene.  The
    // loader has already disarmed every increment without the SH field (and INC-1
    // without the adaptive gate that alone can produce a release edge); resolving it
    // here as well is belt-and-braces so a hand-built NetworkConfig (tests) cannot arm
    // a response to an event that cannot happen.  `dw_hygiene_on` is the single
    // predicate the seam and the assembly's held-row lookup read.
    {
        const bool field = config_.estimateNetworkIonoSh();
        const auto& idw = config_.getIonoConstraintDeweightConfig();
        k.dw_release_reseed = field && nc.iono_dw_release_reseed && idw.adaptive;
        k.dw_release_eps    = (nc.iono_dw_release_floor_eps > 0.0)
                              ? nc.iono_dw_release_floor_eps : 1e-9;
        k.dw_park_bound_tecu = field ? std::max(0.0, nc.iono_dw_park_bound_tecu) : 0.0;
        // Every screen and bound standardizes the harvested T5 deviation, so they need
        // that channel; the loader has already disarmed them without it.
        const bool dev = field && config_.getGlobalVtecShConfig().rep_error_tau_s > 0.0;
        k.dw_park_bound_tecu = dev ? k.dw_park_bound_tecu : 0.0;
        k.dw_release_validate_k =
            dev ? std::max(0.0, nc.iono_dw_release_validate_k) : 0.0;
        k.dw_admit_validate_k =
            dev ? std::max(0.0, nc.iono_dw_admit_validate_k) : 0.0;
        k.dw_hygiene_on = k.dw_release_reseed || k.dw_park_bound_tecu > 0.0
                       || k.dw_release_validate_k > 0.0
                       || k.dw_admit_validate_k > 0.0;
    }
    // Redundancy / testability census.  Nothing
    // to resolve against the estimation configuration — the census reads the
    // solved system and writes counters, so the two arming knobs ARE the
    // predicate.  The loader has already clamped the thresholds.
    k.redun_census_on = nc.redundancy_census_stride > 0
                     || nc.redundancy_escalate_sigma > 0.0;
    // Baarda reliability numbers / MDB.
    // Nothing to resolve against the estimation configuration either — MDB reads
    // (sigma_i, r_i) pairs the host loops already hold and writes counters — but the
    // two SOURCE flags are resolved here rather than re-derived at the feed sites,
    // for two reasons.  (1) The RAIM source needs the tier it rides to be live:
    // raim_redundancy_normalize alone computes no r_i, because networkPostFitRaim is
    // not called at all without network.raim.  (2) The run-end invariant-#4 lines
    // must be able to say WHICH feed was armed when a source contributed zero rows —
    // "the RAIM pool never ran warm" and "no sweep completed" demand different fixes.
    // The AND with a live source is belt-and-braces against the loader's hard error,
    // so a hand-built NetworkConfig (tests) cannot arm a flush over dead wiring.
    k.mdb_from_raim   = config_.networkRaim() && config_.networkRaimRedundancyNormalize();
    k.mdb_from_census = k.redun_census_on;
    k.mdb_on          = nc.mdb && (k.mdb_from_raim || k.mdb_from_census);
    k.mdb_event_m     = std::max(0.0, nc.mdb_event_m);
    k.mdb_event_holdoff = static_cast<long long>(std::max(0, nc.mdb_event_holdoff_ep));
    // Prior-residual census.  Nothing to
    // resolve against the estimation configuration — the census reads
    // (prior mean, prior var, posterior var) triples the carry harvests already
    // hold and writes counters, so the master arm IS the predicate.  The family
    // CSV is parsed ONCE, here, into per-family bools: the feed sites sit in the
    // serial carry harvest and must test a bool, not search a vector, per row.
    // `prires_on` is the AND of the arm and a non-empty KNOWN family set, so a
    // census armed with only unknown family names (the loader already WARNed)
    // never enters a single feed site.
    {
        k.prires_on = false;
        if (nc.prior_residual_census) {
            for (const std::string& f :
                 NetworkCarryVce::parseComponents(nc.prior_residual_families)) {
                if      (f == "pl")   k.prires_pl   = true;
                else if (f == "pb")   k.prires_pb   = true;
                else if (f == "cb")   k.prires_cb   = true;
                else if (f == "zwd")  k.prires_zwd  = true;
                else if (f == "iono") k.prires_iono = true;   // QUEUE 8b slant-iono carry
                else if (f == "rxcb") k.prires_rxcb = true;
                else if (f == "clk")  k.prires_clk  = true;   // Card Q3 INC-0
            }
            k.prires_on = k.prires_pl || k.prires_pb || k.prires_cb
                       || k.prires_zwd || k.prires_iono || k.prires_rxcb || k.prires_clk;
        }
        // The corroboration join is re-AND-ed with the master here rather than
        // trusted from the loader's hard error: belt-and-braces, so a hand-built
        // NetworkConfig (tests) cannot arm a join behind a census that never runs.
        k.prires_corrob       = k.prires_on && nc.prior_residual_corroborate;
        k.prires_report_sigma = (nc.prior_residual_report_sigma > 0.0)
                                ? nc.prior_residual_report_sigma : 4.0;
        k.prires_event_holdoff = std::max(0, nc.prior_residual_event_holdoff_ep);
        // Constraint-aware global σ̂₀.
        // Same belt-and-braces AND with the master.  `prires_s0g_redun` mirrors
        // `mdb_from_raim` EXACTLY and for the same reason: raim_redundancy_
        // normalize alone computes no r_i, because networkPostFitRaim is not
        // called at all without network.raim.  Resolved here rather than
        // re-derived at the feed site so the r ≡ 1 fallback is decided ONCE and
        // the run-end line can say which mode the whole run was in.
        k.prires_s0g       = k.prires_on && nc.prior_residual_global_sigma0;
        k.prires_s0g_redun = config_.networkRaim()
                          && config_.networkRaimRedundancyNormalize();
    }
    // BlockFormal LOS block census.  The master is
    // AND-ed with a LIVE HOST here rather than trusted from the loader's hard
    // error — belt-and-braces, so a hand-built NetworkConfig (tests) cannot arm a
    // sweep behind a redundancy census that never runs.  The host predicate is the
    // census's own two-term expression, repeated verbatim as every other consumer
    // repeats it; there is no `network.redundancy_census` bool to read.
    {
        k.bb_on     = nc.block_baarda && k.redun_census_on;
        k.bb_corrob = k.bb_on && nc.block_baarda_corroborate;
        k.bb_report_sigma = (nc.block_baarda_report_sigma > 0.0)
                            ? nc.block_baarda_report_sigma : 4.0;
        k.bb_event_holdoff =
            static_cast<long long>(std::max(0, nc.block_baarda_event_holdoff_ep));
    }
    // Station congruence screen.  Same host
    // predicate, same belt-and-braces AND, and for the same reason the block
    // census has one: a hand-built NetworkConfig (tests) must not be able to arm
    // a sweep behind a redundancy census that never runs.  `cg_on` is ALSO what
    // gates the assembly's INC-0 geometry retention, so resolving it here — once,
    // before ctx.knobs goes const — is what keeps the producer and the assembly
    // from disagreeing about whether row_pos_enu exists.
    {
        k.cg_on = nc.congruence_screen && k.redun_census_on;
        k.cg_report_sigma = (nc.congruence_report_sigma > 0.0)
                            ? nc.congruence_report_sigma : 4.0;
        k.cg_event_holdoff =
            static_cast<long long>(std::max(0, nc.congruence_event_holdoff_ep));
    }
    // Prefit census / structural fuse / seed-birth tripwire / topology ledger /
    // flight recorder.  Nothing to
    // resolve against the estimation configuration either — these read the
    // assembled row stacks and (for the fuse) cap a weight, so the arming knobs
    // ARE the predicate; the loader has already clamped them.  `pc_walk_on` is
    // the UNION of the three walk-riding increments: INC-3 and INC-4 need the
    // classification pass whether or not the INC-1 columns are wanted.
    k.pc_census_on     = nc.trace_prefit_census;
    k.pc_event_sigma   = std::max(0.0, nc.prefit_event_sigma);
    k.pc_event_mask    = parseDataClassMask(nc.prefit_event_classes);
    k.pc_event_holdoff = static_cast<long long>(std::max(0, nc.prefit_event_holdoff_ep));
    k.pc_fuse_sigma    = std::max(0.0, nc.prefit_fuse_sigma);
    k.pc_fuse_mask     = parseDataClassMask(nc.prefit_fuse_classes);
    k.pc_birth_sigma   = std::max(0.0, nc.seed_birth_check_sigma);
    k.pc_topo_on       = nc.trace_topology;
    k.pc_walk_on       = k.pc_census_on || k.pc_birth_sigma > 0.0 || k.pc_topo_on;
    // H-rider (edge-queue §1.6 item 3a): the census-fed H σ̂₀.  The loader has
    // already forced it off without trace_prefit_census (the born-cell deferral
    // reads the census walk's presence map) and without emit_epoch_status_record
    // (no H records to feed).
    k.h_sigma0_census      = config_.networkHSigma0PrefitCensus();
    k.h_sigma0_class_mask  = parseDataClassMask(config_.networkHSigma0Classes());
    k.rec_on           = nc.rhs_attrib_on_trip;
    // Datum graph census.  Nothing to resolve
    // against the estimation configuration: the census reads the epoch's
    // registration sets and the admitted support, and writes nothing back, so
    // the master arm IS the predicate.  Deliberately NOT AND-ed with
    // `sbasis_complete`: without SBC the spectral side still supplies the
    // observed-vs-declared COUNT, the reconciliation degrades to count-level and
    // the trace says so (`obs_src=count`) — silently disarming the census there
    // would hide the tier-1 fragmentation reading, which needs no spectrum at all.
    k.dg_on            = nc.datum_graph_census;
    k.dg_event_holdoff = static_cast<long long>(std::max(0, nc.datum_graph_event_holdoff_ep));
    // Product-referenced measurement admission.  Nothing to resolve against the
    // estimation configuration either — the screen reads `observed` / `modeled`
    // and removes observables, so the arming knobs ARE the predicate and the
    // loader has already clamped the thresholds.  `adm_on` is the UNION of the
    // response and the census because both ride the SAME pass; screening implies
    // the census columns, which is why `adm_trace` is resolved as the
    // OR rather than copied.  The clamps are repeated here rather than trusted:
    // belt-and-braces, so a hand-built NetworkConfig (tests) cannot arm a screen
    // on a bound of zero and reject the entire network.
    k.adm_screen   = nc.adm_screen;
    k.adm_trace    = nc.adm_trace || nc.adm_screen;
    k.adm_on       = k.adm_screen || k.adm_trace;
    k.adm_bound_m  = (nc.adm_bound_m > 0.0) ? nc.adm_bound_m : 1000.0;
    k.adm_min_sats = std::max(2, nc.adm_min_sats);
    k.adm_heal_ep  = std::max(1, nc.adm_heal_ep);
    k.adm_max_frac = (nc.adm_max_frac > 0.0 && nc.adm_max_frac < 1.0)
                     ? nc.adm_max_frac : 0.5;
    // INC-4 re-entry vet.  The `&& adm_screen` is the loader's INERT-guard
    // re-imposed at the predicate rather than trusted from load order — a vet is an
    // ADMISSION verdict and the measure-only arm makes none, so a census-only run
    // that somehow reached here with `adm_revet` set must not start rewriting arc
    // records.  Same belt-and-braces reason as the clamps above.
    k.adm_revet        = nc.adm_revet && nc.adm_screen;
    k.adm_revet_gap_ep = std::max(1, nc.adm_revet_gap_ep);
    k.adm_revet_ep     = std::max(0, nc.adm_revet_ep);
    k.adm_revet_census = nc.adm_revet_census;
    k.adm_revet_hold   = nc.adm_revet_hold;
    // Resolved to METRES here, once, so the seam compares against a number rather
    // than re-deriving a product per channel per epoch.  The clamp is repeated
    // rather than trusted (a hand-built NetworkConfig in a test could carry a
    // scale of 0 and refuse every re-entry in the network); a scale above 1 would
    // make the "tighter" vet bound looser than the card's own bound, which
    // inverts the knob, so it clamps too.
    {
        const double s = (nc.adm_revet_bound_scale > 0.0
                          && nc.adm_revet_bound_scale <= 1.0)
                             ? nc.adm_revet_bound_scale : 0.2;
        k.adm_revet_bound_m = k.adm_bound_m * s;
    }
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
    const bool   carry_invariant_pd_safe = ctx.knobs.carry_invariant_pd_safe;
    const bool   pl_on = ctx.knobs.pl_on;
    const double pl_q_amb = ctx.knobs.pl_q_amb;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const double pl_max_gap_s = ctx.knobs.pl_max_gap_s;
    const double pl_var_floor = ctx.knobs.pl_var_floor;
    const bool   pl_los_scale = ctx.knobs.pl_los_scale;   // F1: per-LOS B_eff scale at inject
    const bool   pl_local_block = ctx.knobs.pl_local_block; // F3: station-wide whitened local block at inject
    const double pb_var_floor = ctx.knobs.pb_var_floor;
    const double cb_var_floor = ctx.knobs.cb_var_floor;
    const bool   zwd_on = ctx.knobs.zwd_on;
    const double zwd_q = ctx.knobs.zwd_q;
    const bool   rx_cb_on = ctx.knobs.rx_cb_on;
    const double rx_cb_q = ctx.knobs.rx_cb_q;
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
        // Carrier-phase wind-up MODEL REVISION.  Unlike every other token here
        // this is a CODE constant, not a knob — and it has to be, because the
        // thing that changes what the carried state means is a source fix, not a
        // config.  The pre-2026-08-21 dipole sign left +2*phi_wu in every phase
        // observation (~0.4 cyc ZD arc range), which the carried ambiguities,
        // the phase-leveling zero-q level and the shared PB/CB carry all
        // absorbed.  Resuming a snapshot written under the old convention into a
        // corrected binary would re-reference stiff carries levelled in a frame
        // that no longer exists — standing invariant #1 (a stale zero-q value
        // carried past a jump) and #7 (a re-coupling edge with nothing
        // re-validating what it reconnects).  Nothing else in this fingerprint
        // can see it: it hashes config knobs only, with no code version or
        // binary hash.  Appending the token forces a COLD start across the fix.
        add(kWindUpYawModelId);
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
        // The Eq-34 chain datum references a different carried-ambiguity
        // frame (per-receiver RPB split + spanning-tree pins) than the anchor/pivot
        // or ensemble datums; a resume across a toggle flip must cold-start.
        // Appended ONLY when armed, so every non-eq34 snapshot stays byte-identical.
        if (phase_datum_eq34) add("phdatum:eq34");
        // Card 4 OPEN-4: the invariant carry snapshots a DD z-store the legacy
        // carries don't have; a resume across a carry_invariant flip must cold-start.
        // Appended ONLY when armed, so every non-carry snapshot stays byte-identical.
        if (carry_invariant) add("carry:inv");
        // PD-safe DD fold: the carried Pᶻ a pd_safe run
        // writes came out of a DIFFERENT recursion — product-form posterior, equilibrated
        // factorization, eigen-clamp repairs, degenerate blocks rebirthed rather than
        // frozen — and is PD by construction.  Resuming one arm's z-store into the other
        // mixes the two arithmetics inside a single lineage, which is exactly the
        // "never flip mid-chain" failure the fingerprint exists to prevent.  Appended
        // ONLY when armed, so every pre-card snapshot's fingerprint is untouched.
        // (An OFF-run snapshot restored INTO a pd_safe run is separately handled: the
        // INC-2c restore guard rebirths any non-PD block it finds in the blob.)
        if (carry_invariant_pd_safe) add("carry:pdsafe");
        // network.carry_aware_covariance is deliberately ABSENT from the fingerprint.  It
        // is a READ-back knob: its two consumers (dd_sigma_cyc, the log-only AR probe) feed
        // nothing back, so no byte of the snapshot — carry blob, stash, emitter state —
        // changes meaning when it flips.  Fingerprinting it would strand every existing
        // snapshot for a trace column (the loader's fingerprint-SAFE trace-knob
        // precedent — the trace-class knobs that stay armable on a warm resume
        // against an untraced snapshot, in GnssConfigLoadNetwork.cpp).  If a
        // later card routes the correction into the phase-leveling stash (the stored prior
        // variance), THAT knob is a different knob and it MUST be fingerprinted.
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        // F1 (network.phase_leveling_los_scale): the stash is unchanged but the
        // inject-side WEIGHTING of the same stashed variances differs across the
        // flip (the carried level was accumulated under a different recursion),
        // so a resume across it must cold-start — the pl_var_floor rule.  Appended
        // ONLY when armed, so every pre-F1 snapshot fingerprint is unchanged.
        if (pl_los_scale) add("pl:losscale");
        // F3 (network.phase_leveling_local_block): same rule, stronger reason — the
        // carried LEVELS were accumulated under a different prior (joint vs
        // independent rows), and the harvest block itself is not in the snapshot.
        // Armed-only, beside the F1 tag.
        if (pl_local_block) add("pl:losblock");
        // B9-in-block (network.carry_zwd_in_block): solve-affecting for the same reason
        // F3 itself is — the carried ZWD *and* the carried levels were accumulated under a
        // different prior geometry (one joint Σ over {ambiguities, ZWD} vs a block plus an
        // independent diagonal row), so a warm resume must not cross the flip.  The
        // harvest block is not snapshotted either way.  Armed-only append, beside the F1/F3
        // tags, so every pre-card snapshot fingerprint is unchanged.
        if (config_.networkCarryZwdInBlock()) add("zwd:blk");
        // I-in-block (network.carry_iono_in_block): solve-affecting for the same reason —
        // the carried levels were accumulated against a prior geometry that either did or
        // did not include the station's iono columns in the joint Σ, so a warm resume must
        // not cross the flip.  The in-block iono carry itself is not snapshotted (it
        // cold-starts), but the LEVELS that resume are the ones its presence shaped.
        // Armed-only append, beside the F1/F3/ZWD tags, so every pre-card fingerprint is
        // unchanged.  q and the gap join it because both change the prior's weight.
        if (config_.networkCarryIonoInBlock()) {
            add("iono:blk");
            add(fmt::format("{}", config_.networkCarryIonoInBlockQM2()));
            add(fmt::format("{}", config_.networkCarryIonoInBlockMaxGapS()));
        }
        // RX-CLK-in-block (network.carry_rx_clock_in_block): solve-affecting for the same
        // reason — the carried levels were accumulated against a prior geometry that either
        // did or did not include the station's receiver-clock column in the joint Σ, so a
        // warm resume must not cross the flip.  The clock carry itself is not snapshotted
        // (it cold-starts, one epoch of today's white-clock behaviour), but the LEVELS that
        // resume are the ones its presence shaped.  Armed-only append, beside the
        // F1/F3/ZWD/iono tags, so every pre-card fingerprint is unchanged.  The whole
        // variance ladder joins it: each of q, rate_q, rate_var0 and the ISB flag changes
        // the prior's weight, and jump_k changes which epochs carried anything at all.
        if (config_.networkCarryRxClockInBlock()) {
            add("rxclk:blk");
            add(fmt::format("{}", config_.networkCarryRxClockQM2()));
            add(fmt::format("{}", config_.networkCarryRxClockMaxGapS()));
            add(fmt::format("{}", config_.networkCarryRxClockRateVar0()));
            add(fmt::format("{}", config_.networkCarryRxClockRateQ()));
            add(fmt::format("{}", config_.networkCarryRxClockJumpK()));
            add(fmt::format("{}", config_.networkCarryRxClockIsb()));
        }
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
        // Queue 1(b): the handover S-transform changes WHICH carried priors fold
        // at a pivot switch, so a resume across a toggle change would interpret a
        // carry that was (or was not) re-expressed in the wrong datum.  Appended
        // ONLY when armed, so every pre-existing snapshot fingerprint is
        // unchanged (byte-identical OFF path) — the phase_datum_ensemble /
        // phase_leveling_los_scale precedent.  ARMED ⇒ fresh convergence only.
        if (ctx.knobs.handover_stransform) add("sts:1");
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        // EXT-CB: the satellite code bias moves from a shared COLUMN to a modelled
        // CORRECTION on the observable, and the forced rx DCB adds a local state —
        // so a carried shared border means something different with it armed than
        // without, and a warm resume must not cross the flip.  The (sat_code_bias,
        // iono_sh) pair above already reads (false, true) only in this posture, but
        // that is an inference from a coupling; append the knob itself so the
        // fingerprint states it.  Conditional-append (the rxcb/shl2/iono2 pattern)
        // so every pre-card snapshot fingerprint is unchanged.
        if (config_.networkExternalSatCodeBias()) add("extcb:1");
        // RXCB-DATUM changes which receiver-DCB combination is realized, so the
        // carried rx-CB levels mean something different with it armed.  Same
        // conditional-append rule: pre-card fingerprints are unchanged.
        if (config_.networkRxCodeBiasDatum())
            add(fmt::format("rxcbdatum:{}", config_.networkRxCodeBiasDatumSigmaM()));
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // Slant-iono carry (QUEUE 8b): solve-affecting — the leveling levels a chain
        // harvests under iono-carried solves mean something different than white-iono
        // ones, so a warm resume must not cross the flip even though the carry map
        // itself is not snapshotted (it cold-starts).  Conditional-append (the
        // rxcb/shl2/iono2 pattern) so every pre-card snapshot fingerprint is unchanged.
        if (config_.networkCarryIono())
            add(fmt::format("ionocarry:{}:{}:{}:{}:{}", config_.networkIonoCarryScope(),
                            config_.networkIonoCarrySource(),
                            config_.networkIonoCarryQ(), config_.networkIonoCarryVarFloor(),
                            config_.networkIonoCarryMaxGapS()));
        // rx-CB carry: conditional append ONLY when armed, so every
        // pre-existing snapshot fingerprint is unchanged (the shl2/iono2 pattern).
        if (rx_cb_on) {
            add("rxcb:1"); add(fmt::format("{}", rx_cb_q));
            // Card LV-2: the de-mean changes
            // what a carried rx-CB level MEANS — the carry stops transporting the
            // common mode, so a chain that folded demeaned priors and one that did
            // not have realized different receiver-DCB frames.  Same conditional-
            // append rule (nested inside rxcb:1, which is itself conditional), so
            // every pre-card snapshot fingerprint is unchanged.
            if (ctx.knobs.rx_cb_demean) {
                add(fmt::format("rxcbdemean:{}", ctx.knobs.rx_cb_demean_min_sta));
                // Card LV-5: a chain that folded against THIS epoch's μ and one that
                // folded against last epoch's realized different receiver-DCB frames.
                if (ctx.knobs.rx_cb_demean_within) add("rxcbdemeanwin:1");
            }
            // Card LV-4: which priors fold, and which carried entries survive an
            // event, both change — so a resume must not cross the flip.  Same
            // nested conditional-append rule.
            if (ctx.knobs.rx_cb_step_gate)
                add(fmt::format("rxcbstep:{}", ctx.knobs.rx_cb_step_k));
            // The S-consistent harvest re-zeros the carried set's common mode every
            // epoch, so a stored snapshot's absolute rx-CB levels live in a
            // different frame than an untransformed chain's — a resume must not
            // cross the flip.  Same nested conditional-append rule.
            if (ctx.knobs.rx_cb_harvest_demean)
                add(fmt::format("rxcbharv:{}", ctx.knobs.rx_cb_harvest_screen_z));
        }
        // The anchor changes the TEMPORAL
        // MODEL of the carried satellite clock — a snapshot written by an anchored run
        // carries variances produced by a different process, and its per-satellite
        // ANCHORED/LOOSE state deliberately does not travel (a warm resume is an
        // invariant-#1 event, so every satellite must re-earn the anchor).  Mixing the
        // two inside one lineage would fold a stiffness whose evidence never existed.
        // Conditional append, so every pre-card snapshot fingerprint is unchanged.
        if (ctx.knobs.cta_on)
            add(fmt::format("cta:{}:{}:{}:{}:{}:{}:{}:{}", ctx.knobs.cta_q,
                            ctx.knobs.cta_var_floor, ctx.knobs.cta_max_gap_s,
                            ctx.knobs.cta_k, ctx.knobs.cta_abs_m,
                            ctx.knobs.cta_sigma_floor_m, ctx.knobs.cta_recouple_ep,
                            ctx.knobs.cta_resolve));
        // §2.1 gauge-free fold: a snapshot written by a gauge-free run carries a
        // carry whose clock information lives on a different SUBSPACE, so a resume
        // must not cross the flip.  Conditional-append so a plain-anchor lineage's
        // fingerprint is unchanged by this card.
        // Card C-GF: the token now follows the RESOLVED predicate, so arming the
        // downdate through the carry knob invalidates a snapshot exactly as arming it
        // through the Q3 knob always did — the carried block's information lives on a
        // different subspace either way, and a resume that crossed the flip would fold
        // incompatible carries.  Byte-identical for every existing config: with
        // shared_carry_gauge_free off, carry_gauge_free == cta_on && cta_gauge_free.
        // Closure is part of the token, not a separate one: it changes WHICH directions
        // are marginalized out of the carried block, so a resume that crossed a closure
        // flip would fold a carry living on a different subspace — the same
        // incompatibility the bare gauge-free flip causes.  "cta:gf" alone is preserved
        // for the legacy arming so existing snapshots still match.
        if (ctx.knobs.carry_gauge_free)
            add(ctx.knobs.carry_gauge_free_closure ? "cta:gf+cl" : "cta:gf");
        // §2.1 bisection: the denominator decides the TRIP population, and a trip
        // withdraws a prior and re-solves the epoch — so two runs differing only here
        // produce different carried state and must not resume across each other.
        // Appended only when the resolved pair is NOT what `auto` would have chosen,
        // so an existing lineage's fingerprint is unchanged by the knob's arrival.
        if (ctx.knobs.cta_on
            && (ctx.knobs.cta_stat_proj_post != ctx.knobs.cta_gauge_free
                || ctx.knobs.cta_stat_proj_prior))
            add(fmt::format("cta:gfstat:{}:{}", ctx.knobs.cta_stat_proj_post ? 1 : 0,
                            ctx.knobs.cta_stat_proj_prior ? 1 : 0));
        // Card LV-1: the seed reframe changes the per-station LINEARIZATION POINT of
        // the per-LOS iono AND (through initializeCodeBiases, which subtracts the
        // seeded iono) of the receiver DCB.  The absolute solve is invariant to that
        // frame, but the SNAPSHOT stores absolute carried levels whose prefit
        // innovations are read against x_lin — the rx-CB inject gate is exactly such
        // a consumer — so a resume must not cross the flip.  Conditional append.
        if (ctx.knobs.lv1_seed_frame)
            add(fmt::format("lv1seed:{}", ctx.knobs.lv1_min_sats));
        // M2 v2: the variance FACTOR multiplies every
        // carry fold variance, so a carried (value, var) pair means a different prior
        // with it armed than without — the textbook fingerprint case, and the reason
        // this knob is NOT in the carry_vce monitor's fingerprint-free class.  Note
        // the fingerprint is necessary but NOT sufficient here: both legs of an
        // apply-ON chain fingerprint identically while α² itself is unsnapshotted, so
        // networkTryWarmResume refuses the resume outright (spec §8, the one BLOCKING
        // deferral).  Appended ONLY when armed, so every pre-card snapshot's
        // fingerprint is untouched.
        if (config_.networkCarryVceApply()) {
            add("vceapply:1");
            add(config_.networkCarryVceApplyFamilies());
            add(fmt::format("{}", config_.networkCarryVceApplySlew()));
            add(fmt::format("{}", config_.networkCarryVceApplyMax()));
            add(fmt::format("{}", config_.networkCarryVceApplyMinRows()));
            add(fmt::format("{}", config_.networkCarryVceComponents()));
            add(fmt::format("{}", config_.networkCarryVceWindowEpochs()));
        }
        // apriori_source + anchor σ are datum-relevant.  The carried clock
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
        // The conditional local carry changes what the per-station carry MEANS
        // (a shared-coupled conditional row vs an independent diagonal marginal) — a
        // resume across a toggle change would fold a carry of the wrong KIND, and that
        // alone is why the token exists.  §18.12 AMENDMENT: the per-column coupling data
        // (M_r rows, ŝ_prev, P_prev) IS persisted now, but only into a format-v3 snapshot
        // written under network.state_snapshot_coupling_capture and only rebuilt by a
        // reader that arms network.resume_coupling_restore; a v1/v2 file still carries
        // none of it and a restored row degrades to a plain diagonal.  So the rationale is
        // the MEANING change, no longer "the data is missing" — the token stays either
        // way (deleting it would strand every stored seed).
        // Conditional-append (only when armed) keeps every existing fingerprint byte-identical.
        if (carry_cpl_cond)
            add(fmt::format("carrycplcond={}", carry_cpl_cond));
        // The time-linked code-bias datum changes what the emitted OSB/clock
        // split MEANS (a time-continuity datum vs a per-epoch set-mean datum) and its
        // per-(const,signal) previous-value map is NOT persisted in the snapshot — a
        // resume across a toggle change would compare a solve in one datum against a
        // carry in the other.  Conditional-append (only when armed) keeps every
        // existing fingerprint byte-identical.
        if (cb_time_linked)
            add(fmt::format("cbtimelink={}", cb_time_linked));
        // The border time update changes what an injected coupling row MEANS
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
        // Card 1f/1g: both change WHICH information an injected coupling row carries —
        // 1f alters the coefficient vector of every truncated row, 1g moves whole columns
        // onto the departed route.  Same footing as carry_cpl_tu above; conditional-append
        // keeps every existing fingerprint byte-identical.
        if (ctx.knobs.carry_cpl_gauge)
            add(fmt::format("carrycplgauge={}", ctx.knobs.carry_cpl_gauge));
        if (ctx.knobs.carry_cpl_reinit)
            add(fmt::format("carrycplreinit={}", ctx.knobs.carry_cpl_reinit));
        // JOINT-whitening reshapes the injected coupling rows (departed cols →
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
        // --- DELIBERATELY NOT FINGERPRINTED: the forward-only-policy exemption ---
        //
        // Everything listed below is left OUT of the fingerprint on one shared
        // argument, recorded here so a future reader does not "fix" the omission.
        //
        // The rule: a knob is exempt when it is a FORWARD-ONLY POLICY — it changes
        // what the run DOES from here on (which rows fold, at what weight, which
        // observables are admitted, which arcs re-tap, what a resume rebuilds) but
        // re-references no carried quantity retroactively and changes what no
        // serialized state MEANS.  A carried ambiguity, code bias, slant or ZWD is
        // the same quantity in the same frame whether or not a screen is deciding
        // what feeds it next epoch.  Each such knob's OWN state is per-epoch or
        // per-resume and is never serialized, so a resumed leg simply starts with
        // it cold — the conservative direction, since a resume is exactly a window
        // in which no time-difference detector can operate.
        //
        // Why it matters practically: a fingerprinted knob cannot be A/B'd on one
        // snapshot.  Keeping these out is what makes every OFF/ON validation pair
        // replay from a shared seed under `state_snapshot_require: true`.
        //
        // Exempt families:
        //   eq34 reattach hygiene   eq34_anchor_min_arc_ep, phase_datum_time_linked,
        //                           eq34_fresh_pin_sigma_scale
        //   signal integrity        network.signal_integrity.*
        //   iono-deweight release   iono_dw_{release_reseed,park_bound_tecu,
        //                           release_validate_k}
        //   measurement admission   adm_{screen,bound_m,min_sats,heal_ep,max_frac,
        //                           trace} and adm_revet{,_gap_ep,_ep,_census,_hold}
        //   carry hygiene           phase_anchor_carry_reref
        //   slip response           cycle_slip.{relevel_free_epoch,relevel_delta_bridge}
        //   resume response         zwd_seam_reseed, resume_frame_restore,
        //                           resume_slip_warmup_ep, resume_coupling_restore,
        //                           resume_carry_rebase, resume_d5_warmup_ep,
        //                           resume_datum_restore (RS-3, §21.7),
        //                           resume_emit_restore (RS-5, §21.10),
        //                           resume_station_restore{,_rxcb,_shdev} (RS-6, §21.13),
        //                           resume_border_restore (RS-7, §21.14)
        //
        // Two entries need their own note:
        //
        //   adm_revet reads a cross-epoch record, which LOOKS fingerprint-relevant.
        //   It is not: `vet_left` / `last_ep` live in the same unserialized record
        //   map as `admitted`, so a resumed leg opens no vet windows and every
        //   channel reads as a first sighting — the restore case, already handled.
        //   (`adm_resume_pending` makes that first restored epoch a screened RESTORE
        //   rather than a birth, the one case the seed-birth tripwire cannot see.)
        //
        //   state_snapshot_datum_capture (RS-3) is the same representation question with
        //   one twist: its rows ride the EXISTING header (the optional-var_first
        //   precedent), so a pre-card binary rejects the file on the unknown record TAG
        //   rather than on the header literal.  Both are this format's documented
        //   cold-start signal and both throw under state_snapshot_require, so the safety
        //   is identical; keeping the header stable is what lets an RS-3 A/B diff two
        //   snapshots and see only the new rows.  The datum quantities themselves are
        //   already fingerprinted by the knobs that create them (phase_datum_eq34,
        //   code_datum_time_linked), so a writer/reader mismatch on their MEANING is
        //   unreachable.
        //
        //   state_snapshot_coupling_capture is a serialization REPRESENTATION, not a
        //   policy: it adds rows and bumps the header to v3.  That is a binary
        //   vintage question — a pre-card reader fails the header check and
        //   cold-starts — not a fingerprint one, and the writer's products and traces
        //   stay byte-identical because the capture block reads ctx.carry after the
        //   emit and writes only into `snap`.  The Card-1 arithmetic tokens those
        //   rows depend on are already in fp above, so a writer/reader mismatch on
        //   the coupling arithmetic is unreachable.  The same precedent covers the
        //   unconditional Z-row and frame-row captures, and the RS-5 emit-layer
        //   (state_snapshot_emit_capture) and RS-6 station-layer
        //   (state_snapshot_station_capture) captures: both add rows under the
        //   EXISTING header and leave the writer's products and traces byte-identical,
        //   and the quantities themselves are already fingerprinted by the knobs that
        //   create them (carry_invariant_emit_rebase / emit_pb_filter for the emit
        //   layer; carry_rx_code_bias and global_vtec_sh.rep_error_tau_s for the two
        //   station carries), so a writer/reader mismatch on their MEANING is
        //   unreachable.
        //
        // ⚠ Do NOT move any of these into the fingerprint: it would strand every
        //   stored snapshot AND outlaw the same-snapshot OFF/ON A/B that their
        //   validation depends on.  (zwd_on / zwd_q above DO stay fingerprinted —
        //   the continuous carry is solve-affecting on both sides of a seam.)
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
    }
}

void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                          const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                          NetworkInvariantCarryDriver& carry_driver,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkClockEmitRebase* clk_rebase,
                          NetworkEmitRebase* emit_rebase,
                          NetworkPbEmitFilter* pb_filter) {
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
    //
    // ⚠ The message is built by NetworkStateSnapshotIO::warmResumeRefusalMessage so
    // every branch below says the same thing in the same words, and the
    // non-positive-age branch (the one SPEC_event_harness §6b found reading as a
    // COLD START in the log and then killing the process with an unhandled
    // exception) names its own remedy.  The throw is caught at the runNetwork()
    // call site, which logs e.what() as the ONE error line and returns 1 — a
    // controlled abort with the reason in the log, not a 0xC0000409 fast-fail.
    auto hb1 = [&](WarmResumeRefusal why, const std::string& detail) {
        const std::string msg = NetworkStateSnapshotIO::warmResumeRefusalMessage(
                nc.state_snapshot_require, why, detail);
        if (!msg.empty()) throw std::runtime_error(msg);
    };
    // The COLD-START warnings below are suppressed under `require`: announcing a
    // cold start the run is about to refuse is the exact confusion §6b reported.
    const bool cold_ok = !nc.state_snapshot_require;
    if (nc.state_snapshot_require && !snap_path.empty()
        && !std::filesystem::exists(snap_path))
        hb1(WarmResumeRefusal::MissingFile,
            "snapshot file '" + snap_path.string() + "' does not exist");
    // M2 v2 — the ONE field-BLOCKING deferral,
    // enforced rather than documented.  The variance factor α² is cross-epoch,
    // solve-affecting, and NOT in the snapshot; a resumed leg would restart every
    // group at α² = 1 and re-climb at the slew rate, so the carry priors either
    // side of the seam encode different variances for the same carried values.
    // The fingerprint cannot catch this (an apply-ON seed and an apply-ON leg
    // fingerprint identically), so the refusal has to live here.  v1 is
    // fresh-convergence-only; the relaxation is a snapshot field plus
    // NetworkCarryVce::seedRobustFloor, and neither exists yet.
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
            WarmResumeRefusal gate_why = WarmResumeRefusal::None;
            if (first_live_s < 0.0) {
                if (cold_ok)
                    log->warn("Network (RTB-7): state snapshot '{}' found but there are no live "
                              "epochs to resume against — COLD START.", snap_path.string());
                hb1(WarmResumeRefusal::NoLiveEpochs,
                    "no live epochs to resume against (snapshot '" + snap_path.string() + "')");
            } else if (!NetworkStateSnapshotIO::warmResumeAllowed(
                           snap, live_fingerprint, first_live_s, horizon_s, &gate, &gate_why)) {
                if (cold_ok)
                    log->warn("Network (RTB-7): state snapshot '{}' NOT warm-resumable — {} — "
                              "COLD START (a new generation id is minted at first emit as usual).",
                              snap_path.string(), gate);
                hb1(gate_why, "snapshot '" + snap_path.string() + "': " + gate);
            } else {
                // Emitter layer FIRST (all-or-nothing): a failed emitter restore
                // discards the whole warm resume — restoring the driver carries
                // without the emitter's baselines/pending flags would emit WRONG
                // discontinuity counters (the silent consumer datum break).
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
                        // Iono-2 dual-layer: the second shell's coefficients ride the
                        // (empty-for-VtecSh) sat field as "L1" — a layer tag old
                        // readers key-mismatch into a safe climatology reseed.
                        if (cp.kind == SharedParamKind::VtecSh && cp.sat == "L1") {
                            cp.sh_layer = 1;
                            cp.sat.clear();
                        }
                        prev_carry.push_back(std::move(cp));
                    }
                    // C1: the D records serialize per-key (value, var) only, so the
                    // carried block's CROSS-covariance comes from the QK/QP side-car or
                    // not at all.  Restored (RS-7 / §18.12 (2a)) the first warm fold runs
                    // the dense path; otherwise it runs the legacy per-key diagonal and
                    // the dense block re-seeds at the first emit.
                    //
                    // ⚠ This line is printed BEFORE the dense-border block below, so it
                    // must not assert "diagonal" on a leg that is about to restore the
                    // border — invariant #4 cuts both ways, and P9a (§21.14) reads exactly
                    // this clause.  The predicate is the block's own arming test minus the
                    // post-filter nk>0 / finite screen, which only the block can evaluate;
                    // if that screen fails the block warns and says it degraded to the
                    // diagonal, so the pair of lines stays truthful either way.
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
                    for (const auto& [key, l] : snap.local_carry) {
                        // RESTORED-ARC AGE (SPEC_event_harness §15.4): the birth
                        // stamps are deliberately not snapshotted, and the entry is
                        // therefore SENTINEL-stamped rather than left at 0 — 0 also
                        // means "never stashed", which is what the harvest's
                        // first-stash-wins rule reads it as, so the first post-resume
                        // harvest used to re-brand every restored arc as born at the
                        // RESUME.  var_first stays 0 on a pre-ratio file: the ratio
                        // ceiling then refuses the arc until it re-converges.
                        local_carry[key] = makeRestoredLocalCarry(
                            l.value_abs, l.var, l.last_seen_s, l.var_first);
                    }
                    // STABILITY target 2 leg C: seam-scoped soft refloat of the
                    // restored leveling carries.  The h-vintage zero-q levels were
                    // leveled under the OLD run's ZWD realization; against the
                    // resume's fresh station filters they are an invariant-#1
                    // stiff prior (measured: the post-retire ZWD drag, §18).  A
                    // one-shot variance ADD keeps the carried value as the prior
                    // mean — no refloat-from-code, no fresh station-common-iono
                    // seeding — and lets the fold re-earn the stiffness.  RESUME
                    // path only: continuous epochs structurally never reach here.
                    if (ctx.knobs.amb_seam_reinflate_var > 0.0 && !local_carry.empty()) {
                        for (auto& [key, l] : local_carry)
                            l.var += ctx.knobs.amb_seam_reinflate_var;
                        log->info("Network (amb seam re-inflate): +{:.3e} m² added to {} restored "
                                  "leveling carr{} at the resume seam (one-shot soft refloat; "
                                  "values kept as prior means).",
                                  ctx.knobs.amb_seam_reinflate_var, local_carry.size(),
                                  local_carry.size() == 1 ? "y" : "ies");
                    }
                    zwd_carry.clear();
                    for (const auto& [idx, l] : snap.zwd_carry)
                        // Same restored-entry convention as the L rows above.  The ZWD
                        // carry has no age reader today and its harvest replaces the
                        // whole entry (so the sentinel does not survive the first
                        // stash), but a restored Z row IS a restored entry and reads
                        // as one — an age gate added here later must not have to
                        // rediscover §15.4.
                        zwd_carry[idx] = makeRestoredLocalCarry(l.value_abs, l.var,
                                                                l.last_seen_s);
                    // Seam-scoped ZWD re-seed (SPEC_synthetic_minimum_network §17
                    // finding 3): arm the post-resume inject window.  This is the
                    // RE-COUPLING edge (invariant #7): the restored h-vintage ZWD
                    // posteriors reconnect to the solve for a bounded horizon while
                    // the (deliberately un-snapshotted) station filters re-establish
                    // their own memory; the retire at horizon exhaustion is the
                    // DECOUPLING edge, logged at the harvest.  The window rides the
                    // ordinary B9 fold — per-entry staleness (pl_max_gap_s) and the
                    // carry_warm gate still apply per epoch.
                    if (ctx.knobs.zwd_seam_on) {
                        if (!zwd_carry.empty()) {
                            ctx.carry.zwd_seam_ep_left = ctx.knobs.zwd_seam_epochs;
                            log->info("Network (ZWD seam re-seed): ARMED at the resume seam — {} "
                                      "restored Z row(s) will fold as B9-style priors for the next "
                                      "{} emitted epoch(s) (RW q {:.3e} m²/s over the true gap), "
                                      "then retire.",
                                      zwd_carry.size(), ctx.knobs.zwd_seam_epochs,
                                      ctx.knobs.zwd_q);
                        } else {
                            log->warn("Network (ZWD seam re-seed): warm resume completed but the "
                                      "snapshot carried ZERO Z rows (written by a pre-capture "
                                      "build?) — the seam window is INERT this run; the ZWD "
                                      "restarts exactly as an unarmed resume.");
                        }
                    }
                    // Resume FRAME restore (network.resume_frame_restore, SPEC_synthetic_
                    // minimum_network §18.10).  The RE-COUPLING edge (invariant #7): the
                    // realization-frame bookkeeping the v1 snapshot omitted — per-(sat,
                    // signal) phase-anchor incumbents + R2 sticky pin targets, the Q3
                    // clock-anchor per-sat state (with its t_last, so the stale test runs
                    // on the TRUE gap), and the CLK-R held frame — reconnects to the solve
                    // BEFORE the first election / clock fold / emit.  Measured without
                    // it: the election runs incumbent-less for every family, marks
                    // them all, erases every restored leveling carry, marginalizes their
                    // D2 PB prior and re-pins the frame at fresh code seeds; every clock
                    // folds LOOSE for the re-couple horizon; the held frame restarts at 0.
                    // Identities are counted (invariant #4: 0 under an armed knob = INERT).
                    if (ctx.knobs.resume_frame_restore) {
                        if (!snap.hasFrameState()) {
                            log->warn("Network (resume frame restore): ARMED but the snapshot "
                                      "carries NO frame rows (written by a pre-v2 build?) — "
                                      "INERT this run; the first election re-pins cold exactly "
                                      "as an unarmed resume.");
                        } else {
                            ctx.carry.anchor_incumbent  = snap.anchor_incumbent;
                            ctx.carry.anchor_pin_target = snap.anchor_pin_target;
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
                                ctx.carry.clk_anchor.sat[sat] = st;
                                ++ctx.counters.n_resume_frame_cta;
                                if (ca.anchored) ++ctx.counters.n_resume_frame_cta_anchored;
                            }
                            // Judge graft (Card 3 → the built card): PRIME the two-edge
                            // handover ledger from the restored leveling rows, so a
                            // family whose restored incumbent no longer tracks the
                            // satellite at the first live epoch takes the CONTINUOUS
                            // handover path (NetworkPhaseDatum.cpp re-validate gate on
                            // the restored conditional variance) instead of the cold
                            // pin + 1(d) mark + erase.  The ledger is rebuilt wholesale
                            // by the first post-resume harvest, so this is a one-epoch
                            // seed exactly like the ordinary ledger lifetime.  Only when
                            // the continuous-handover machinery is armed (the map is
                            // unread otherwise).
                            std::size_t n_ledger_primed = 0;
                            if (ctx.knobs.pd_handover_continuous) {
                                for (const auto& [key, lc] : ctx.carry.local_carry) {
                                    if (!(lc.var > 0.0) || !std::isfinite(lc.value_abs)) continue;
                                    AnchorHandoverLedger::Entry e;
                                    e.value = lc.value_abs;
                                    e.var   = lc.var;
                                    e.var_first = lc.var_first;
                                    ctx.carry.handover.value_abs[{std::get<0>(key),
                                        std::get<1>(key) + "_" + std::get<2>(key)}] = e;
                                    ++n_ledger_primed;
                                }
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
                    // RS-3 DATUM restore (network.resume_datum_restore, SPEC_event_harness
                    // §21.7).  The frame restore above brought back the eq34 tree's
                    // SATELLITE side (anchor_incumbent); this brings back the receiver
                    // side and the second, unrelated datum memory, BEFORE anything that
                    // could run an election — the whole of networkTryWarmResume runs
                    // ahead of the first epoch, so the ordering is structural, and the
                    // loader guarantees resume_frame_restore is armed with it.
                    //
                    // Measured without it at the field seam: the sticky-tree seed sees an
                    // empty eq34_rx_parent / eq34_sat_root, rebuilds the tree from the
                    // cold arc registry, and every phase-bias family re-realizes by the
                    // integer between the old and the new pinned arc — 110
                    // PHASE_DATUM/eq34_reref at the first post-resume epoch against the
                    // seed's 2, an R record naming 30 satellites, 14 m per-satellite PB
                    // offsets.  The DD branch cannot prevent it (a DD is invariant to the
                    // datum by construction), which is why RS-2's branch-faithful resume
                    // left the wire's realization untouched.
                    //
                    // The two halves are gated and reported SEPARATELY: a file may carry
                    // one and not the other (a run with phase_datum_eq34 but no
                    // code_datum_time_linked writes no CB* rows and vice versa), and an
                    // absent half must read as INERT for that half alone, never borrow
                    // the other's verdict (the hasFrameState/hasCouplingState precedent).
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
                            ctx.carry.eq34_datum_rx  = snap.eq34_datum_rx;
                            ctx.carry.eq34_rx_parent = snap.eq34_rx_parent;
                            ctx.carry.eq34_sat_root  = snap.eq34_sat_root;
                            ctx.carry.eq34_prev_target.clear();
                            for (const auto& [key, tgt] : snap.eq34_prev_target)
                                ctx.carry.eq34_prev_target[{key.first, key.second}] = tgt;
                            ctx.carry.eq34_prev_el.clear();
                            for (const auto& [key, el] : snap.eq34_prev_el)
                                ctx.carry.eq34_prev_el[{static_cast<std::size_t>(key.first),
                                                        key.second}] = el;
                            ctx.carry.eq34_retained_edges.clear();
                            for (const auto& [idx, amb] : snap.eq34_retained_edges)
                                ctx.carry.eq34_retained_edges.emplace(idx, amb);
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
                            ctx.carry.code_bias_prev_      = snap.code_bias_prev;
                            ctx.carry.code_bias_sat_seen_  = snap.code_bias_sat_seen;
                            // ...and the pair those values were solved under.  Restoring
                            // the values without the pair is strictly WORSE than today:
                            // `pair_same` still reads false everywhere, so the IF-pin RHS
                            // still re-seeds to 0, while the restored seen-set now makes
                            // every one of those re-seeds BUMP.  A pair that genuinely
                            // changed across the seam still trips the test and still
                            // bumps — the resume's previous kept epoch IS the writer's.
                            ctx.carry.code_bias_ifpin_pair_ = snap.code_bias_ifpin_pair;
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
                    // RS-5 EMIT-LAYER restore (network.resume_emit_restore,
                    // SPEC_event_harness §21.10).  AFTER the frame and datum restores
                    // and before anything that could emit — the whole of
                    // networkTryWarmResume runs ahead of the first epoch, so the
                    // ordering is structural, and the loader guarantees
                    // resume_frame_restore is armed with it (an emit layer restored on
                    // top of a RE-ELECTED realization frame would re-base this run's
                    // biases onto a level the writer never emitted).
                    //
                    // Measured without it at the field seam (§21.10): the leg's emit
                    // layer is 0.000 on ALL 129 keys at the first post-resume epoch
                    // while the seed's is 18.410 m p50 at the same tow, and the emitted
                    // difference closes EXACTLY as raw + (leg layer − seed layer) over
                    // 120 keys — i.e. the whole 17.666 m step is these two cold starts,
                    // not the estimator (whose own state moves 1.695 m).
                    //
                    // The two halves are gated and reported SEPARATELY, the
                    // hasFrameState / hasDatumState precedent: a file may carry the ER
                    // rows and no PF/PS (a writer with carry_invariant but without
                    // emit_pb_filter), and an absent half must read as INERT for that
                    // half alone.
                    if (ctx.knobs.resume_emit_restore) {
                        const bool have_er = !snap.emit_rebase_last.empty();
                        const bool have_pf = !snap.pb_filter_state.empty()
                                             || !snap.pb_filter_step.empty();
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
                        if (have_pf && pb_filter) {
                            NetworkPbEmitFilter::FilterState fs;
                            for (const auto& [key, ks] : snap.pb_filter_state)
                                fs.state[key] = NetworkPbEmitFilter::KeyState{
                                    ks.mean, ks.var, ks.last_seen_s};
                            for (const auto& [cst, ac] : snap.pb_filter_step)
                                fs.step[cst] = NetworkPbEmitFilter::StepAccum{
                                    ac.sum_log_e2, ac.n_eff};
                            pb_filter->restoreState(fs);
                            ctx.counters.n_resume_emit_filter_keys =
                                static_cast<long long>(snap.pb_filter_state.size());
                            ctx.counters.n_resume_emit_step_accums =
                                static_cast<long long>(snap.pb_filter_step.size());
                        } else {
                            log->warn("Network (RS-5 emit restore): ARMED but {} — the PB "
                                      "emission FILTER half is INERT; every key "
                                      "first-sights at the seam (seeded at the raw value, "
                                      "P = R) and σ̂_step restarts at the weak seed prior, "
                                      "so the first post-resume epochs run the widest gate "
                                      "of the run.",
                                      have_pf
                                          ? "this run never constructs the filter engine "
                                            "(network.emit_pb_filter off)"
                                          : "the snapshot carries NO PF/PS rows (a writer "
                                            "without network.state_snapshot_emit_capture, "
                                            "or one with the filter off)");
                        }
                        if ((have_er && emit_rebase) || (have_pf && pb_filter))
                            log->info("Network (RS-5 emit restore): ARMED at the resume "
                                      "seam — {} emission-frame key(s) restored into the "
                                      "eq34 emit re-base, {} per-key posterior(s) and {} "
                                      "step-noise accumulator(s) into the PB emission "
                                      "filter.  Restored BEFORE the first emit, so the "
                                      "first post-resume epoch re-bases onto the writer's "
                                      "last EMITTED values instead of passing the raw "
                                      "solve-frame value through; outputs-only, no "
                                      "estimate moves.",
                                      ctx.counters.n_resume_emit_rebase_keys,
                                      ctx.counters.n_resume_emit_filter_keys,
                                      ctx.counters.n_resume_emit_step_accums);
                    }
                    // RS-6 STATION-LAYER restore (network.resume_station_restore,
                    // SPEC_event_harness §21.13).  AFTER the emit restore and before the
                    // first epoch — the whole of networkTryWarmResume runs ahead of it,
                    // so the ordering is structural, and the loader guarantees
                    // resume_frame_restore is armed with it (both memories are expressed
                    // in the realization frame underneath them).
                    //
                    // Measured without it at the field seam (§21.13): the rx-CB inject
                    // gate withholds 26 % of its priors on the leg against 13 % on the
                    // seed — the whole station layer re-seeding from one epoch of code —
                    // and ~400 deviation entries cold-seed at +1, which is the SH a₀₀
                    // transient.
                    //
                    // The two halves are gated and reported SEPARATELY, the
                    // hasFrameState / hasDatumState / hasEmitState precedent: a file may
                    // carry RC rows and no SD (a writer with the rx-CB carry and no T5
                    // deviation carry), and an absent half must read as INERT for that
                    // half alone.  The two sub-switches are DIAGNOSTIC — they let one
                    // §21.13 attribution leg restore a single half, and a withheld half
                    // is logged as withheld, never as absent.
                    if (ctx.knobs.resume_station_restore) {
                        const bool have_rc = !snap.rx_cb_carry.empty();
                        const bool have_sd = !snap.sh_dev_carry.empty();
                        if (have_rc && ctx.knobs.resume_station_rxcb) {
                            // RESTORED-ARC AGE (§15.4.1), exactly as the L / Z rows:
                            // the entry goes back through makeRestoredLocalCarry rather
                            // than a bare aggregate, so a restored rx-CB entry is
                            // distinguishable from a never-stashed one at every reader
                            // that grows an age test later.  The rx-CB harvest itself
                            // stashes a bare LocalCarry and its inject reads only
                            // value_abs / var / last_seen_s, so the sentinel changes
                            // nothing THIS card measures — it is the convention, and an
                            // age gate added here later must not have to rediscover it.
                            ctx.carry.rx_cb_carry.clear();
                            for (const auto& [key, l] : snap.rx_cb_carry)
                                ctx.carry.rx_cb_carry[{key.first, key.second}] =
                                    makeRestoredLocalCarry(l.value_abs, l.var,
                                                           l.last_seen_s);
                            ctx.counters.n_resume_station_rxcb =
                                static_cast<long long>(ctx.carry.rx_cb_carry.size());
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
                        if (have_sd && ctx.knobs.resume_station_shdev) {
                            // Restored AS WRITTEN — no sentinel, and none is wanted:
                            // ShDevCarry's own cold test is `t_s <= 0`, so carrying the
                            // writer's harvest stamp is what makes the entry read WARM,
                            // and the epoch's aging pre-pass then ages it over the TRUE
                            // gap (and drops it at 5·tau, the existing staleness rule).
                            ctx.carry.sh_dev_carry.clear();
                            for (const auto& [key, d] : snap.sh_dev_carry)
                                ctx.carry.sh_dev_carry[key] =
                                    ShDevCarry{d.dev, d.var, d.var_ss, d.t_s};
                            ctx.counters.n_resume_station_shdev =
                                static_cast<long long>(ctx.carry.sh_dev_carry.size());
                        } else {
                            log->warn("Network (RS-6 station restore): ARMED but {} — the "
                                      "T5 deviation half is INERT; every per-LOS entry "
                                      "cold-seeds at the steady state (dev 0, var "
                                      "representation_sigma²) at the seam, which is the "
                                      "SH a₀₀ transient.",
                                      have_sd
                                          ? "network.resume_station_restore_shdev is false "
                                            "(a §21.13 attribution leg)"
                                          : "the snapshot carries NO SD rows (a writer "
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
                    // Coupling regressors (network.resume_coupling_restore) — the
                    // second half of the frame restore, and the RE-COUPLING edge for
                    // the leveling carry itself.
                    //
                    // Without it a restored LocalCarry has an empty `coupling`, so
                    // buildCplRow's per-entry loop never runs: row.shared is empty,
                    // rhs = value_abs − x_lin, σ² = var_eff.  That is a plain DIAGONAL
                    // prior at the PURE CONDITIONAL variance, which no continuous epoch
                    // produces — the diagonal carry is stiff at the MARGINAL variance,
                    // while the coupling row is stiff at the conditional one but
                    // SHARED-NEUTRAL, its A_s = M_r cancelling the reduced-normals
                    // contribution.  In the Schur reduction (A_r = e_c, A_s = M, weight
                    // w, P = N_rr⁻¹, n_c = e_cᵀPe_c, M_d = N_sr P e_c, α = w/(1+w·n_c))
                    // the shared normals gain α(M − M_d)(M − M_d)ᵀ.  An empty coupling
                    // is the M = 0 case, driving the border by the full α‖M_d‖ at
                    // α ≈ 1e6 m⁻² per row — four orders above the anchored clock prior's
                    // ≈ 1.1e4 m⁻², and the source of the 33 first-fold trips.
                    //
                    // ⚠ Touches DIRECTION only; `var` is never modified.  The variant
                    // that attacked the row's WEIGHT walked the consumer 5.13 m in h12
                    // by letting the level re-float on the cold station layer
                    // (invariant #1, the slip-relevel-walk family).
                    //
                    // All-or-nothing per row, guaranteed upstream: the parser refuses
                    // any file with incomplete, out-of-range, non-finite or orphaned
                    // rows, so a row reaching here is bit-for-bit the write epoch's
                    // harvest, in harvest order.  A row with no QL/QZ record keeps its
                    // empty coupling and degrades to the diagonal.  Coefficients are NOT
                    // re-sparsified and ŝ_prev / P_prev are NOT re-derived: P_prev = 0 is
                    // the meaningful "datum-pinned, exactly known" value, and re-deriving
                    // it would activate the κ shrink on a column the datum re-applies
                    // exactly.
                    //
                    // Deliberately OUTSIDE the resume_frame_restore block: it rides its
                    // own knob, so frame-ON / coupling-OFF stays byte-for-byte the old
                    // path in the new binary — the same-build control invariant #6
                    // requires.  hasFrameState() is also the wrong INERT predicate here,
                    // and the frame block's ledger sub-block carries a third gate this
                    // must not inherit.
                    if (ctx.knobs.resume_coupling_restore) {
                        // Gate on the population the restore actually REBUILDS, not on
                        // hasCouplingState() (which is true for a QR- or QS-only file too):
                        // otherwise a file with no QL/QZ takes the else branch and logs
                        // "ARMED ... 0 L row(s) + 0 Z row(s)" while seeding tu_reinit_hist —
                        // the invariant-#4 'inert reported as armed' shape.
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
                                        cs.coeff    = mj;           // must NOT resolve to
                                        cs.s_prev   = q.s_prev;     // the layer-0 column
                                        cs.p_prev   = q.p_prev;
                                        cpl.push_back(std::move(cs));
                                    }
                                    lc.coupling = std::move(cpl);
                                };
                            long long n_rows = 0, n_zrows = 0, n_terms = 0, n_miss = 0;
                            for (const auto& [key, ents] : snap.coupling_rows) {
                                const auto it = local_carry.find(key);
                                if (it == local_carry.end()) { ++n_miss; continue; }
                                rebuild(ents, it->second);
                                ++n_rows;
                                n_terms += static_cast<long long>(ents.size());
                            }
                            for (const auto& [idx, ents] : snap.coupling_zrows) {
                                const auto it = zwd_carry.find(idx);
                                if (it == zwd_carry.end()) { ++n_miss; continue; }
                                rebuild(ents, it->second);
                                ++n_zrows;
                                n_terms += static_cast<long long>(ents.size());
                            }
                            ctx.counters.n_resume_cpl_dict =
                                static_cast<long long>(snap.coupling_dict.size());
                            ctx.counters.n_resume_cpl_rows    = n_rows;
                            ctx.counters.n_resume_cpl_zrows   = n_zrows;
                            ctx.counters.n_resume_cpl_terms   = n_terms;
                            ctx.counters.n_resume_cpl_orphan  = n_miss;
                            ctx.carry.resume_cpl_census_pending = (n_rows + n_zrows) > 0;

                            // ---- The DECOUPLING edge the snapshot cannot otherwise
                            // reach (standing invariant #7).  ctx.carry.tu_reinit_hist is
                            // per-epoch state and never serialized, so on the first warm
                            // epoch no restored term would be diverted for a column that
                            // was demonstrably being RE-DRAWN at the write epoch.  Seed
                            // it from the three sources the snapshot does carry, at a
                            // stamp strictly above every restored last_seen_s so
                            // tuReinitSince fires for exactly the restored population.
                            // The term then takes the DEPARTED route (κ = 0, M²·P_prev
                            // marginalized into σ², coefficient handed to the gauge
                            // restore) instead of folding a re-drawn value at full weight.
                            //
                            // Deliberately NOT seeded: a pivot change, a sticky re-pin, a
                            // Σz=0 re-realization, or a per-epoch set-mean code-bias datum
                            // step.  Those are PURE GAUGE motions, and the identity
                            // Mᵀg_s = −g_r(c) makes a COMPLETE row exactly invariant under
                            // them — the solution moves as (r + αg_r, s + αg_s) and the row
                            // residual is unchanged.  Blanket-diverting them would inflate
                            // σ² on ~40 % of every row's terms for no correctness gain.
                            // carry_coupling_gauge_project, loader-required here, repairs
                            // the family sum for whatever DOES truncate.
                            //
                            // The fourth source, prev_carry.inflate_q, needs no seed: it
                            // is restored on the D rows and the first warm epoch's serial
                            // pre-pass writes it at pl_now_s, already above every restored
                            // stamp.
                            double t_seed = 0.0;
                            for (const auto& [k2, lc] : local_carry)
                                t_seed = std::max(t_seed, lc.last_seen_s);
                            for (const auto& [i2, lc] : zwd_carry)
                                t_seed = std::max(t_seed, lc.last_seen_s);
                            t_seed = std::nextafter(t_seed,
                                         std::numeric_limits<double>::infinity());
                            long long n_seed = 0, n_loose = 0;
                            const auto seedClock = [&](const std::string& sat) {
                                ctx.carry.tu_reinit_hist[{SharedParamKind::Clock, sat,
                                                          std::string{}}] = t_seed;
                                ++n_seed;
                            };
                            for (const auto& [sat, ca] : snap.clock_anchor)
                                if (!ca.anchored) { seedClock(sat); ++n_loose; }
                            for (const std::string& sat : snap.eph_cutover_disc_pending)
                                seedClock(sat);
                            // QR: a hard-refloated sat is skip-stashed, so restoring the set
                            // is the only way the first warm epoch's pre-pass
                            // (ApplicationNetworkAssembly.cpp:2320-2326) behaves as a
                            // continuous epoch's would.
                            ctx.carry.prev_clock_refloat_sats = snap.clock_refloat_sats;
                            ctx.counters.n_resume_cpl_seed_reinit = n_seed;

                            log->info("Network (resume coupling restore): ARMED at the resume "
                                      "seam — {} L row(s) + {} Z row(s) rebuilt their Card-1 "
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
                    // §18.12 (2a): the dense D2 covariance side-car.  TWO read knobs reach
                    // it (RS-7, §21.14).  Under resume_coupling_restore it is the other half
                    // of the coupling seam: the coupling rows are conditional from the
                    // first warm fold, so the border they condition on must fold DENSE
                    // from the first fold too, or the 376-key diagonal asymmetry §18.12
                    // measured stays.  Under resume_border_restore it stands ALONE — a
                    // covariance block carries no regression and no gauge identity, so it
                    // needs neither carry_conditional_coupling nor Card 1f, and that is
                    // what makes it reachable on a carry_invariant field lineage where the
                    // loader refuses resume_coupling_restore outright.  Keys with no
                    // restored D row are dropped (row AND column) and counted; the fold
                    // still degrades per-key as before.
                    //
                    // Invariant #4: armed with no usable QK/QP records (a writer without
                    // network.state_snapshot_coupling_capture, or a truncated block) the
                    // knob is INERT and has to SAY so — a border that silently restarts
                    // diagonal IS the defect this card exists to close.
                    if (ctx.knobs.resume_border_restore && fullcov_on && !border_rows_present)
                        log->warn("Network (RS-7 border restore): ARMED but the snapshot "
                                  "carries NO usable QK/QP records ({} key(s), {} row(s)) — "
                                  "a writer without network.state_snapshot_coupling_capture, "
                                  "or a truncated block.  INERT this run: the dense shared "
                                  "border re-seeds from the first emitted epoch exactly as "
                                  "before, which IS the §21.14 defect.",
                                  snap.carry_cov_keys.size(), snap.carry_cov_rows.size());
                    if (border_restore_armed && border_rows_present) {
                        // Name the knob that actually armed the restore: on a field leg
                        // resume_coupling_restore is refused by the loader, so a line
                        // reading "resume coupling restore" would send the reader hunting
                        // a knob the config does not carry.
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
                    // §21.14 (invariant #4): the first-fold census — n_resume_cpl_diag_border,
                    // the count of carried keys that missed prev_carry_cov.row and folded
                    // through the legacy per-key DIAGONAL path — is the ONLY instrument that
                    // reads whether the border came back dense.  It used to be latched
                    // inside the coupling restore, so a resume that did not arm THAT knob
                    // reported 0 for the same reason a healthy one does, and the control arm
                    // of any border A/B was unreadable.  Latch it for every warm resume that
                    // has a dense carry at all: the CONTROL legs then read the full diagonal
                    // count and the border legs read ~0 (plus whatever the drop screen
                    // rejected).  Trace-class — everything the latch gates is a counter or a
                    // measure-only census vector, so products stay byte-identical.
                    if (fullcov_on && !prev_carry.empty()) {
                        ctx.carry.resume_cpl_census_pending = true;
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
                    // RS-2b (§21.6): the same window one layer up, on the ESTIMATION-side
                    // fine-slip gate.  Armed HERE and nowhere else — a cold start must never
                    // set it, or the gate would be diagnostic-only on a run with no restored
                    // branch to protect.  Countdown decremented at the install seam.
                    if (ctx.knobs.resume_d5_warmup_ep > 0) {
                        ctx.carry.resume_d5_warmup_left = ctx.knobs.resume_d5_warmup_ep;
                        log->info("Network (RS-2b resume D5 warm-up): ARMED — the Card-4 D5 "
                                  "innovation gate acts diagnostic-only for the next {} "
                                  "processed epoch(s): trips are counted, never re-tapped and "
                                  "never marked slipped.  The σ̂₀ feed, the v3 corroboration "
                                  "gate and the MW/LLI/IF-TD drains are untouched.",
                                  ctx.knobs.resume_d5_warmup_ep);
                    }
                    ura_sigma0_ewma     = snap.ura_sigma0_ewma;
                    ura_refloat_horizon = snap.ura_refloat_horizon;
                    ura_retro_sats      = snap.ura_retro_sats;
                    retro_active        = snap.retro_active;
                    // Restore the eph-cutover re-level baseline + any pending
                    // forced disc so a warm resume does not re-level against a stale
                    // baseline (or lose a pending forced bump).  Empty in an sp3 or
                    // pre-Card-6 snapshot ⇒ the first brdc_rts epoch re-seeds cleanly.
                    relevel_iode             = snap.relevel_iode;
                    eph_cutover_disc_pending = snap.eph_cutover_disc_pending;
                    // The time-linked code-bias previous-value map is NOT
                    // persisted, so it re-seeds empty here.  A satellite emitted before
                    // the snapshot whose split now re-seeds IS a re-reference the
                    // consumer must see — mark the run "resumed" so every first post-
                    // resume re-seed disc-bumps (conservative; a genuinely-new sat's
                    // first emit absorbs a harmless bump).  Inert unless code_datum_time_linked.
                    //
                    // RS-3 (§21.7): unless the DATUM restore above actually brought the map
                    // back.  Then the datum CONTINUES across the seam — a satellite whose
                    // split is unchanged has no step to signal — so the blanket latch would
                    // manufacture 30 consumer-visible discontinuities the wire does not
                    // have.  The per-satellite seen-set restored with it keeps the
                    // distinction the latch was standing in for: a satellite that genuinely
                    // re-seeds is still a RETURN and still bumps.
                    code_bias_datum_resumed_ = !datum_cb_restored;
                    // RS-3 telemetry latch: measure the first post-resume epoch's datum
                    // churn (eq34 rerefs / reattaches, code-bias re-seed discs) on EVERY
                    // warm resume, armed or not, so an A/B's control leg is read by the
                    // same instrument as its treatment.  Pure counters — nothing
                    // downstream branches on it, so the OFF path stays byte-identical
                    // (the anchor_age_resume_seed / adm_resume_pending shape).
                    ctx.carry.resume_datum_first_ep = true;
                    // RS-4 (§21.9): the DECLARATION half's one-shot latch, set beside the
                    // RS-3 telemetry latch and for the same reason — every warm resume
                    // arms it so an A/B's control leg is read by the same instrument.
                    // NetworkEmitStore consumes it at the FIRST emit, the only point at
                    // which the emitter's restored per-key baselines still hold the
                    // previous run's emitted values.  Read only under the knob.
                    ctx.carry.resume_pb_seam_pending = true;
                    // eq34 reattach hygiene (spec §2 INC-1 warm-resume rule): the
                    // arc-age map is not serialized, so without this latch every arc
                    // would look one epoch old on the first resumed epoch and the
                    // eligibility gate would block EVERY reattach for min_arc_ep
                    // epochs.  Arcs behind a snapshot are mature by definition, so the
                    // first hygiene pass seeds them at the threshold instead of 1.
                    // Inert unless ctx.knobs.anchor_hygiene_on.
                    ctx.carry.anchor_age_resume_seed = true;
                    // Flicker/gap signal integrity (spec ADDENDUM, Aug-1): a warm
                    // resume is the TRUE sibling of the flicker case and the one
                    // situation the card must vet network-wide.  The snapshot restores
                    // TIGHT carries but NO detector state (nothing here deserializes MW
                    // Welford counts or IF-TD arc history — they re-converge from
                    // scratch), so for the first revet_ep epochs every arc in the
                    // network carries fully-warm state with zero time-difference
                    // coverage.  That is the same hazard as a flicker, at network
                    // scale.  Latched as a COUNTDOWN so the classifier can decrement it
                    // without knowing anything about resumes.  Inert unless
                    // ctx.knobs.sig_resume_warmup.
                    ctx.carry.si_resume_warmup_ep = ctx.knobs.sig_resume_warmup
                                                        ? ctx.knobs.sig_revet_ep : 0;
                    // Product-referenced measurement admission (spec §3 INC-1,
                    // second clause): the admission-record map is not serialized, so
                    // on the first post-resume screen pass every channel looks brand
                    // new — but its ESTIMATOR state was restored and may be carrying
                    // the corrupt seed the screen exists to catch.  A restored
                    // corrupt arc is NOT a birth: it must be evented as such and it
                    // must REFLOAT (a birth has nothing to refloat; a restore has a
                    // stiff carried chord that would otherwise ride the hole the
                    // withhold punches — standing invariant #1).  This is the one
                    // case the census card's INC-3 structurally cannot see, and the
                    // reason the spec named it explicitly.  Written unconditionally
                    // (inert unless ctx.knobs.adm_screen), so the OFF path stays
                    // byte-identical — the anchor_age_resume_seed shape.
                    ctx.carry.adm_resume_pending = true;
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
                            hb1(WarmResumeRefusal::Other,
                                "the invariant-carry blob could not be restored/bound: " + cerr);
                        } else {
                            // RE-2: wired event response for the stiff restored carry.
                            std::size_t shocked = 0;
                            if (nc.state_snapshot_resume_shock_m > 0.0)
                                shocked = carry_driver.applyResumeShock(
                                    nc.state_snapshot_resume_shock_m
                                    * nc.state_snapshot_resume_shock_m);
                            log->info("Network (RTB-7c): BRANCH-FAITHFUL — restored invariant "
                                      "DD carry ({} carried DD), {} sat-vertex + {} group-id map "
                                      "entries; sigma0 {}; resume shock {}.  Pᶻ shape: {}.  "
                                      "Linearization frame: {}.",
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
                                                        "geometry resumes exactly"),
                                      // RS-2 / invariant #4: whether the blob carries the ZL
                                      // rows decides whether resume_carry_rebase has anything
                                      // to act on, and nothing else in this line shows it.
                                      carry_driver.resumeCarryLinBlocks() > 0
                                          ? fmt::format("ZL rows present on {} block(s) — the "
                                                        "means can be re-based onto this run's "
                                                        "x_lin ({})",
                                                        carry_driver.resumeCarryLinBlocks(),
                                                        config_.networkResumeCarryRebase()
                                                            ? "resume_carry_rebase ARMED"
                                                            : "resume_carry_rebase off — the "
                                                              "branch folds in the WRITER's "
                                                              "frame")
                                          : std::string("NO ZL rows — the means are in the "
                                                        "WRITER's linearization frame and "
                                                        "cannot be re-based (writer needs "
                                                        "network.state_snapshot_carry_lin)"));
                            // RS-2: armed with nothing to act on is the invariant-#4 case —
                            // the run reads "warm + branch-faithful" and the re-base counters
                            // stay 0 for a reason the log would otherwise never state.
                            if (config_.networkResumeCarryRebase()
                                && carry_driver.resumeCarryLinBlocks() == 0)
                                log->warn("Network (RS-2): network.resume_carry_rebase is armed "
                                          "but the carry blob carries NO ZL rows — the branch "
                                          "is NOT re-based and this resume keeps today's "
                                          "behaviour (every restored chord folds against the "
                                          "WRITER's x_lin).  Re-write the seed with "
                                          "network.state_snapshot_carry_lin.");
                            // PD-safe INC-2c: the
                            // restore guard's loud half.  Every blob written before that
                            // card can carry the corrupt joint lane block (indefinite Pᶻ,
                            // negative diagonals), and importing it verbatim would seed a
                            // fixed build with the frozen carry the fix exists to release.
                            // A non-zero count on the FIRST pd_safe resume is expected;
                            // a non-zero count resuming from a pd_safe-armed run is a NEW
                            // bug — the writing run's blocks were PD by construction.
                            if (carry_driver.resumeCarryRebirthBlocks() > 0)
                                log->warn("Network (Card 4 PD-safe fold, INC-2c): {} restored "
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
                               const std::shared_ptr<spdlog::logger>& log,
                               const NetworkClockEmitRebase* clk_rebase,
                               const NetworkEmitRebase* emit_rebase,
                               const NetworkPbEmitFilter* pb_filter) {
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
            // READMIT: a PARKED entry (a departed satellite's pre-gap realization) is NOT
            // serialized — the D record has no park stamp, so a restored copy would fold as
            // a fresh live carry with epoch-dt inflation (forward-only response policy: a
            // seam inside a satellite's absence loses its seed, i.e. today's cold return).
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
        // network.phase_datum_handover_max_var_ratio: the arc's first-stash variance rides
        // an OPTIONAL trailing L field — filled only when the ratio ceiling is armed, so a
        // default snapshot's L rows stay byte-identical.
        const bool l_var_first = ctx.knobs.pd_handover_max_var_ratio > 0.0;
        for (const auto& [key, lc] : local_carry) {
            NetworkStateSnapshot::Local L{lc.value_abs, lc.var, lc.last_seen_s};
            if (l_var_first) L.var_first = lc.var_first;
            snap.local_carry[key] = L;
        }
        for (const auto& [idx, lc] : zwd_carry)
            snap.zwd_carry[idx] =
                NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
        snap.ura_refloat_horizon = ura_refloat_horizon;
        snap.ura_retro_sats      = ura_retro_sats;
        snap.retro_active        = retro_active;
        snap.relevel_iode              = relevel_iode;              // Card 6
        snap.eph_cutover_disc_pending  = eph_cutover_disc_pending;  // Card 6
        // Resume FRAME state (§18.10, snapshot format v2) — captured when
        // network.state_snapshot_frame_capture is armed (judge graft over the Z-row
        // precedent: a v2 file is unreadable to a pre-card binary, so the default
        // snapshot must stay v1 byte-for-byte): the per-(sat,signal) phase-anchor
        // incumbents + R2 sticky targets as of THIS epoch's election, the Q3 clock-
        // anchor per-sat state (folded sats only), and the CLK-R held frame.
        // Whether a resume USES the rows is the resume-side knob
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
        // Card 1 coupling regressors (§18.12, snapshot format v3) — captured under
        // network.state_snapshot_coupling_capture (its OWN knob, not the frame knob:
        // the rows multiply the file ~37x and the keep-history library with it, and a
        // frame-only writer must stay available for cheap legs and for the arm-B
        // control).  Layout: a DICTIONARY of shared columns keyed on
        // (identity, ŝ_prev, P_prev) plus one packed line per carried row.
        //
        // Why the value pair is part of the dictionary KEY and not just the payload:
        // harvestCoupling assigns cs.s_prev = x_s(j) and cs.p_prev = cov_df(j,j) from
        // the shared COLUMN alone (the harvest lambda in networkHarvestLocalCarries),
        // so rows of ONE harvest epoch agree — but a station whose harvest FAILED hits
        // `continue` before the stash and keeps an OLDER coupling.  Measured on the
        // branch-point file: 12 of 2811 L rows carry an older last_seen_s.  An
        // identity-only hoist would restore those rows against the wrong frame — the
        // frozen-frame class of defect that killed the lane seed site.
        //
        // ROW ATOMICITY (Card 1e): a row is written WHOLE or not at all.  An amputated
        // coefficient vector loses the exact gauge identity Mᵀg_s = −g_r(c); every
        // coupling row then observes the clock gauge and the W5 null-space gate
        // WITHHOLDS each rows-carrying epoch — strictly worse than the diagonal degrade
        // an empty vector already gives.  fmt writes "nan"/"inf" and the parser's strtod
        // would accept and fully consume them, so the finite screen here is
        // load-bearing, not belt-and-braces.
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
                // Screen the WHOLE row BEFORE hoisting anything.  Bailing inside the build
                // loop below would leave the columns already pushed for terms 0..k-1 in
                // snap.coupling_dict as ORPHAN QS records no QL/QZ line references: it
                // inflates snap_cpl_dict_last (the '{}-column dictionary' the run-end line
                // prints), and if EVERY row drops it leaves hasCouplingState() true, so a v3
                // header is written for a file holding a dictionary and zero rows — a shape
                // this format's own prose says cannot occur.  Row atomicity is the stated
                // invariant (Card 1e); it has to cover the dictionary half too.
                for (const CoupledShared& cs : cpl)
                    if (!std::isfinite(cs.coeff) || !std::isfinite(cs.s_prev)
                        || !std::isfinite(cs.p_prev) || cs.p_prev < 0.0)
                        return false;               // omit the WHOLE row, never part of it
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
                        q.sh_layer = cs.sh_layer;   // own field: never the D "L1" hack
                        q.s_prev   = cs.s_prev;
                        q.p_prev   = cs.p_prev;
                        it = dict_id.emplace(
                                 k, static_cast<int>(snap.coupling_dict.size())).first;
                        snap.coupling_dict.push_back(std::move(q));
                    }
                    ents.emplace_back(it->second, cs.coeff);   // HARVEST order preserved
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
            // Card 1d/1g: the refloat set has no D-row carrier (a hard-refloated sat is
            // skip-stashed), so the resume cannot otherwise reproduce the pre-pass mark.
            snap.clock_refloat_sats = ctx.carry.prev_clock_refloat_sats;
            ctx.carry.snap_cpl_dict_last    = static_cast<long long>(snap.coupling_dict.size());
            ctx.carry.snap_cpl_rows_last    = static_cast<long long>(snap.coupling_rows.size());
            ctx.carry.snap_cpl_zrows_last   = static_cast<long long>(snap.coupling_zrows.size());
            ctx.carry.snap_cpl_terms_last   = n_terms;
            ctx.carry.snap_cpl_rows_dropped += n_drop;
            // §18.12 (2a): the C1 dense D2 covariance side-car — the other half of the
            // seam.  Keys in row order (QK), lower triangle per row (QP); the whole block
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
        // RS-3 DATUM capture (network.state_snapshot_datum_capture, SPEC_event_harness
        // §21.7) — the WRITE half of the two datum memories a warm resume had to
        // re-realize.  The frame block above carries the eq34 tree's SATELLITE side
        // (anchor_incumbent); this carries the RECEIVER side plus the per-component root
        // vote, the pin targets, the longevity elevations and the extend-only retained
        // set — together exactly what networkElectChainDatum's sticky seed reads — and
        // the time-linked code-bias previous-value map + ever-seen set.
        //
        // Plain copies of ctx.carry AFTER the emit, into `snap` only: the writer run's
        // products and every trace are byte-identical, which is what makes this knob
        // legal to add to an existing recipe.  Every container is already empty on a run
        // that does not arm the datum it belongs to (phase_datum_eq34 /
        // code_datum_time_linked), so arming the capture on such a run costs nothing and
        // writes no rows.
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
            // elevations are rounded/geometric quantities the election recomputes, while
            // code_bias_prev_ IS the quantity the veteran-referenced sum-to-zero RHS
            // differences against — a nan there would survive the parse (strtod accepts
            // it) and poison the restored datum, so it is dropped at the source.
            for (const auto& [fam, per_sat] : ctx.carry.code_bias_prev_)
                for (const auto& [sat, v] : per_sat)
                    if (std::isfinite(v)) snap.code_bias_prev[fam][sat] = v;
            snap.code_bias_sat_seen = ctx.carry.code_bias_sat_seen_;
            // The IF-pin pair that PRODUCED those values — the third container of the
            // same kept-epoch lifecycle.  Without it the restore is inert on the IF-pin
            // half: `pair_same` reads false for every satellite, the split re-seeds to 0
            // and every satellite disc-bumps (measured: 32 of 32 on a field probe).
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
        // RS-5 EMIT-LAYER capture (network.state_snapshot_emit_capture, SPEC_event_harness
        // §21.10) — the WRITE half of the last cross-epoch state the snapshot did not hold.
        // The frame/datum blocks above carry the ESTIMATOR's realization; these two engines
        // sit downstream of it on the OUTPUTS ONLY (NetworkEmitStore.cpp:164 re-base,
        // :271 filter) and are bare runNetwork locals, so a warm resume cold-starts both
        // and the first post-resume epoch emits the raw solve-frame phase bias while the
        // continuous wire carries the accumulated re-base (17.666 m p50 per (sat,signal)
        // measured at the field seam; §21.10's closure table).
        //
        // Plain exports AFTER the emit, into `snap` only — the same write-side discipline
        // as the datum block, which is what makes this knob legal to add to an existing
        // recipe.  A run that arms neither engine (carry_invariant off / emit_pb_filter
        // off) exports empty maps and writes no rows, so arming the capture there costs
        // nothing.  Nothing here enters the warm-resume fingerprint: the rows change no
        // estimate, only the LEVEL the emitted biases are expressed in.
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
        // RS-6 STATION-LAYER capture (network.state_snapshot_station_capture,
        // SPEC_event_harness §21.13) — the WRITE half of the two per-station memories
        // that restart COLD at every warm resume.  The frame / datum blocks carry the
        // network's realization and the emit block the level on top of it; these two
        // are the STATION layer underneath: the receiver code-bias posterior per
        // (station, CB family) and the per-LOS SH representation-error deviation.
        //
        // Plain copies of ctx.carry AFTER the emit, into `snap` only — the same
        // write-side discipline as the datum and emit blocks, which is what makes this
        // knob legal to add to an existing recipe.  A run that arms neither carry
        // (network.carry_rx_code_bias / the T5 deviation) exports empty maps and writes
        // no rows, so arming the capture there costs nothing.
        //
        // The non-finite screen is at the SOURCE, as for code_bias_prev: a nan that
        // strtod would accept on the way back in becomes the prior the next fold's
        // innovation differences against (rx-CB) or the GM level the next harvest ages
        // (T5).  An entry dropped here is simply cold on the resume — the pre-card
        // behaviour for that one key — where a poisoned one is not recoverable.
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
        }   // state_snapshot_station_capture
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
    // ocean/polar gap is near-null and trips the all-or-nothing rank gate.
    // A degree-dependent Kaula prior (σ_n = kaula_scale/n; a₀₀ a LOOSE climatology
    // prior, so the mean VTEC stays data-determined and is NOT clamped to an
    // external value) keeps every coefficient PSD-bounded
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
        est.attribTag("d2_sh");
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
        est.attribTag("phase_hold");
        est.addSharedPrior(hp);  // skips keys absent from this epoch's layout
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
    }
}

void networkInjectPinBridgePrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                 NetworkEstimator& est, bool within_epoch_armed,
                                 const std::shared_ptr<spdlog::logger>& log) {
    // --- eq34 reattach hygiene INC-2: the time-linked phase datum -----------
    // See the header for why this must be folded PRE-gate.  Inert (no allocation,
    // no row) unless a bridge is open, which requires network.phase_datum_time_linked.
    auto& bridges = ctx.carry.pin_bridge;
    if (!ctx.knobs.pin_bridge_on || bridges.empty()) return;
    std::vector<NetworkEstimator::SharedPrior> bp;
    bp.reserve(bridges.size());
    for (auto& [amb, br] : bridges) {
        // "sat_signal" → (sat, signal).  The satellite id never contains '_', so the
        // FIRST underscore splits it (the StateKeys::parseAmbiguityKey convention —
        // an iono-free combination keeps its own underscores inside `signal`).
        const auto us = amb.find('_');
        if (us == std::string::npos || us == 0 || us + 1 >= amb.size()) continue;
        if (!std::isfinite(br.value)) continue;
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::PhaseBias;
        p.sat    = amb.substr(0, us);
        p.signal = amb.substr(us + 1);
        p.value  = br.value;
        p.info   = ctx.knobs.pin_bridge_info;
        bp.push_back(p);
        ++br.folds;                       // staleness clock (bounded by bridge_max_ep)
        ++ctx.counters.hyg.bridge_folds;
    }
    if (bp.empty()) return;
    est.attribTag("pin_bridge");
    est.addSharedPrior(bp);   // skips keys absent from this epoch's layout
    if (within_epoch_armed)
        ws.cap_priors.insert(ws.cap_priors.end(), bp.begin(), bp.end());
    // Card-KP K2 analogue, sampled ONCE: a datum row whose information sits AT the
    // rank-gate cutoff either trips the gate or survives by coincidence.  The bridge
    // is a DATUM row, so it must sit well above it; report the actual margin rather
    // than trusting the default sigma.  The reduced normals here already contain the
    // row we just folded, which is what the gate is about to judge.
    if (!ctx.counters.hyg.bridge_margin_logged) {
        ctx.counters.hyg.bridge_margin_logged = true;
        const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::EigenvaluesOnly);
        if (es.info() == Eigen::Success && es.eigenvalues().size() > 0) {
            const double lmax = es.eigenvalues()(es.eigenvalues().size() - 1);
            const double cutoff = est.rankGateTol() * std::max(lmax, 1.0);
            const double decades = (cutoff > 0.0 && ctx.knobs.pin_bridge_info > 0.0)
                ? std::log10(ctx.knobs.pin_bridge_info / cutoff) : 0.0;
            ctx.counters.hyg.bridge_margin_decades = decades;
            if (decades >= 2.0)
                log->info("Network (eq34 hygiene INC-2): bridge row info {:.3e} vs "
                          "rank-gate cutoff {:.3e} — margin {:.1f} decades ABOVE the "
                          "cutoff (sigma {:g} m).",
                          ctx.knobs.pin_bridge_info, cutoff, decades,
                          ctx.knobs.pin_bridge_sigma);
            else
                log->warn("Network (eq34 hygiene INC-2): bridge row info {:.3e} sits "
                          "only {:.1f} decades above the rank-gate cutoff {:.3e} — "
                          "BELOW the Card-KP K2 minimum of 2.0.  A datum row at the "
                          "cutoff is counted as a NULL direction, so the gate will "
                          "withhold the very epochs the bridge exists to save.  "
                          "TIGHTEN network.phase_datum_bridge_sigma_m (currently {:g} "
                          "m) or re-place network.rank_gate_tol from this config's "
                          "estimability dump.",
                          ctx.knobs.pin_bridge_info, decades, cutoff,
                          ctx.knobs.pin_bridge_sigma);
        }
    }
}

namespace {

/// READMIT continuity: is @p cp's shared column registered in THIS epoch's layout?
/// (The estimator's lookups throw on an unknown key — the contract resolveSharedCol wraps.)
bool readmitColumnPresent(const NetworkEstimator& est, const CarryPrior& cp) {
    try {
        switch (cp.kind) {
            case SharedParamKind::Clock:     return est.clockColumn(cp.sat) >= 0;
            case SharedParamKind::CodeBias:  return est.codeBiasColumn(cp.sat, cp.signal) >= 0;
            case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cp.sat, cp.signal) >= 0;
            default:                         return false;
        }
    } catch (...) {
        return false;
    }
}

/// READMIT continuity: bookkeeping + the RE-COUPLING-edge event for one column seeded
/// from a parked pre-gap realization at the D2 install (standing invariant #7: the edge
/// has an identity, its evidence gap, and the variance it re-coupled at).
void readmitNoteSeed(NetworkRunContext& ctx, NetworkRunTraces& traces, const GnssTime& t,
                     const CarryPrior& cp, double seed_gap, double horizon_s,
                     double var_eff, bool anchored) {
    const bool clk = cp.kind == SharedParamKind::Clock;
    if (clk) {
        ++ctx.counters.n_readmit_seeded_clk;
        if (anchored) ++ctx.counters.n_readmit_seeded_anch;
        ctx.carry.readmit_seeded_s[cp.sat] = ctx.carry.readmit_now_s;
    } else {
        ++ctx.counters.n_readmit_seeded_cb;
    }
    ctx.carry.readmit_seed_check.push_back({cp.kind, cp.sat, cp.signal, cp.value, var_eff});
    traces.emitEvent(t, "READMIT", clk ? "seed_clock" : "seed_code_bias", cp.sat, cp.signal,
                     seed_gap, horizon_s, "seed",
                     fmt::format("value={:.4f};var_eff={:.3e};anchored={}", cp.value, var_eff,
                                 anchored ? 1 : 0));
}

}  // namespace

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
    // QUEUE item 1(d): the anchored election's re-reference set — consumed below
    // exactly as eq34_reref is (the two are mutually exclusive by election mode,
    // and both are always empty when their mechanism is off).
    const auto& pa_reref = ctx.carry.phase_anchor_reref;
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
    // M2 v2 — the LASER 5-95 variance factor,
    // applied to the shared carry AFTER the hand model has formed var_eff exactly
    // as it does today (q·Δt inflation then the B4 floor).  Both D2 branches route
    // through this one lambda so the diagonal and the C1 dense fold cannot drift
    // apart: the dense branch's DIAGONAL is var_eff, and the marginal prior on any
    // one state must be identical either way — the same property that makes the
    // M2 carry innovation representation-independent in the first place.
    //
    // An inflate_q key is EXCLUDED: it folds with the clock-monitor REINIT variance,
    // a deliberate soft refloat that is not the hand carry model, and the monitor
    // skips those rows too (NetworkStashStatus's feed), so there is no measured
    // factor for them and scaling one would be inventing a number.
    //
    // OFF (or an unarmed family) returns var unchanged ⇒ byte-identical.
    const bool  vce_apply_on = ctx.knobs.vce_apply_on;
    const auto& vce_apply    = ctx.carry.vce_apply;
    const auto vce_scale_shared = [&](SharedParamKind kind, const std::string& sat,
                                      bool inflate_q, double var) {
        if (!vce_apply_on || inflate_q) return var;
        const int slot = (kind == SharedParamKind::PhaseBias) ? NetworkCarryVceApply::kPb
                       : (kind == SharedParamKind::CodeBias)  ? NetworkCarryVceApply::kCb
                                                              : -1;
        return var * vce_apply.factor(slot, sat);
    };
    auto& cap_priors = ws.cap_priors;
    auto& cap_dense_keys = ws.cap_dense_keys;
    auto& cap_dense_cov = ws.cap_dense_cov;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // The guarded temporal clock
    // anchor decides the CLOCK rows' fold variance in both branches below.  The
    // per-satellite elapsed time it inflates over is continuous GPS seconds, NOT
    // the driver's inter-epoch dt (see the seam header — every skip path advances
    // dt while the carry survives).  Untouched, un-allocated when OFF.
    const bool   cta_on    = ctx.knobs.cta_on;
    const double cta_now_s = cta_on ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (cta_on) clockAnchorBeginEpoch(ctx);
    // RTA-2 scope predicate — re-declared verbatim over the explicit
    // (datum_changed, changed_consts) parameters; identical read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };
    // Queue 1(b) INC-1 — the LOAD-BEARING half of the S-transform card.  When the
    // transform re-expressed this constellation's carry (ctx.carry.sts_consts, filled
    // by networkHandoverStransform earlier this epoch), the prior is valid in the
    // INCOMING datum and must FOLD: the retired-datum skip below is released for
    // exactly those keys and the retained rows are counted (invariant #4 — transforms
    // applied with sts_rows_retained == 0 means this release is mis-wired).  The set is
    // empty on every non-handover epoch and whenever the knob is off, so the OFF path
    // is byte-identical.
    const auto sts_released = [&](const std::string& sat) {
        return !ctx.carry.sts_consts.empty() && !sat.empty()
            && ctx.carry.sts_consts.count(sat.front()) > 0;
    };
    // READMIT continuity (E2 seed-first re-admission, network.readmit_seed_horizon_s).
    // The epoch clock the stash (which has none of its own) and the guard read; the
    // seed check list for this epoch; and the frame drop: a clock-datum change retires a
    // parked entry's frame exactly as it skips the live carry of the changed
    // constellation(s) — but a parked entry cannot be re-stashed fresh next epoch, so it
    // is DROPPED (counted), not skipped.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    const double now_s = (readmit_h > 0.0) ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (readmit_h > 0.0) {
        ctx.carry.readmit_now_s = now_s;
        ctx.carry.readmit_seed_check.clear();
        if (datum_changed) {
            const auto before = prev_carry.size();
            prev_carry.erase(std::remove_if(prev_carry.begin(), prev_carry.end(),
                                            [&](const CarryPrior& cp) {
                                                return cp.t_park_s > 0.0
                                                    && handover_affects(cp.sat)
                                                    && !sts_released(cp.sat);
                                            }),
                             prev_carry.end());
            ctx.counters.n_readmit_dropped_datum +=
                static_cast<long long>(before - prev_carry.size());
        }
    }

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
            if (handover_affects(cp.sat)) {
                if (!sts_released(cp.sat)) continue;   // RTA-2: retired-datum key (scoped mode only)
                ++ctx.counters.sts_rows_retained;      // queue 1(b): re-expressed ⇒ folds instead
            }
            // Card 3 (eq34) D4 hygiene: a re-referenced (sat,σ) stepped its b_s
            // frame at this epoch's election — marginalize its carried PB prior
            // for ONE epoch (it belongs to the retired frame; folding it yanks
            // the new frame by up to the full re-level).  Mirrors the RTA-2
            // scoped skip; always empty when eq34 is off.
            if (cp.kind == SharedParamKind::PhaseBias
                && eq34_reref.count({cp.sat, cp.signal})) continue;
            // QUEUE item 1(d): the SAME marginalization for the anchored
            // election's handovers (SPEC_pivot_visibility_gauge_defect §5.07.5
            // R1) — the exact skip this comment block was already describing,
            // extended to the election whose un-transformed carry the satellite
            // clock was measured absorbing.  Always empty when the knob is off.
            if (cp.kind == SharedParamKind::PhaseBias
                && pa_reref.count({cp.sat, cp.signal})) continue;
            if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
            const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                           : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                    : q_pb;
            // READMIT (E2 seed-first re-admission): a PARKED entry is a departed satellite's
            // pre-gap realization.  Column still absent ⇒ nothing to fold (skipped BEFORE
            // the anchor fold, which would otherwise stamp the satellite); column
            // re-registered ⇒ this is the RE-COUPLING edge — the entry seeds it at
            // var + q·gap, the Q3 anchor folds over its own per-satellite elapsed time
            // bounded by the seed horizon, and the guard re-validates; expired ⇒ skipped
            // here, dropped at the stash.  seed_gap == 0 leaves every expression below the
            // legacy one, character for character.
            double seed_gap = 0.0;
            if (cp.t_park_s > 0.0) {
                if (!(readmit_h > 0.0) || !readmitColumnPresent(est, cp)) continue;
                seed_gap = now_s - cp.t_park_s;
                if (!(seed_gap > 0.0) || seed_gap > readmit_h) continue;
            }
            // A clock-monitor-flagged satellite folds with the large REINIT variance (a soft
            // large-q reinitialization): the weak prior still anchors the parameter at its
            // previous value — curbing the collective datum drift a hard refloat causes — but
            // is loose enough for the data to walk a frozen clock back toward truth.
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * (seed_gap > 0.0 ? seed_gap
                                                                         : std::max(0.0, dt));
            // The satellite clock's
            // temporal constraint.  An inflate_q key is deliberately EXCLUDED — that
            // is a monitor-driven soft refloat, not the hand carry model, and
            // overriding it with a stiff anchor would re-freeze exactly what the
            // monitor just loosened.  OFF ⇒ the returned var_eff is the expression
            // above, character for character.
            bool seed_anchored = false;
            if (cta_on && cp.kind == SharedParamKind::Clock && !cp.inflate_q) {
                const ClockAnchorFold cf = clockAnchorFoldVariance(
                    ctx, cp.sat, cp.var, dt, cta_now_s, cp.value,
                    seed_gap > 0.0 ? readmit_h : 0.0);
                var_eff       = cf.var_eff;
                seed_anchored = cf.anchored;
            }
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
            var_eff = vce_scale_shared(cp.kind, cp.sat, cp.inflate_q, var_eff);  // M2 v2 factor
            if (!(var_eff > 0.0)) continue;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;
            if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                clk_prior_info[cp.sat] = p.info;   // B7: scalar clock carry info on this column
            priors.push_back(p);
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
        }
        est.attribTag("d2_carry");
        // Card Q3 §2.1: with the gauge-free fold armed, the CLOCK entries are
        // folded with the per-constellation common direction marginalized out —
        // the anchor then constrains the estimable between-satellite differences
        // and leaves the gauge to the S-basis datum.  Non-clock entries fold
        // identically either way, and the OFF path is the same single call it
        // always was.
        if (ctx.knobs.carry_gauge_free) {
            const std::size_t gf = est.addSharedPriorClockGaugeFree(
                priors, ctx.knobs.carry_gauge_free_closure);
            ctx.counters.cta_gf_keys += static_cast<long long>(gf);
            if (gf == 0) ++ctx.counters.cta_gf_inert;
        } else {
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
        }
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
            if (handover_affects(cp.sat)) {
                if (!sts_released(cp.sat)) continue;   // RTA-2: retired-datum key (scoped mode only)
                ++ctx.counters.sts_rows_retained;      // queue 1(b): re-expressed ⇒ folds instead
            }
            // Card 3 (eq34) D4 hygiene: marginalize re-referenced PB keys for
            // this epoch (retired frame) — see the diagonal branch above.
            if (cp.kind == SharedParamKind::PhaseBias
                && eq34_reref.count({cp.sat, cp.signal})) continue;
            // QUEUE item 1(d): anchored-election handovers, same treatment
            // (see the diagonal branch above).
            if (cp.kind == SharedParamKind::PhaseBias
                && pa_reref.count({cp.sat, cp.signal})) continue;
            if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
            const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                           : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                    : q_pb;
            // READMIT (E2): the SAME parked-entry disposition as the diagonal branch above
            // (a parked entry has no dense row — it was never in the stashed block — so a
            // seed always folds through the diagonal leftovers below).
            double seed_gap = 0.0;
            if (cp.t_park_s > 0.0) {
                if (!(readmit_h > 0.0) || !readmitColumnPresent(est, cp)) continue;
                seed_gap = now_s - cp.t_park_s;
                if (!(seed_gap > 0.0) || seed_gap > readmit_h) continue;
            }
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * (seed_gap > 0.0 ? seed_gap
                                                                         : std::max(0.0, dt));
            // The SAME clock-anchor decision as the diagonal branch above.
            // It must be the same, and it must be on the DIAGONAL of cov_eff: the
            // marginal prior on any one state has to be identical on both branches
            // (the property that makes the guard statistic path-independent), and
            // the dense fold's off-diagonals are retained deliberately — the anchor
            // does not re-create the independence assumption C1 removed on the very
            // state it makes stiffest (SPEC §2.1, assessment deviation #1).
            bool seed_anchored = false;
            if (cta_on && cp.kind == SharedParamKind::Clock && !cp.inflate_q) {
                const ClockAnchorFold cf = clockAnchorFoldVariance(
                    ctx, cp.sat, cp.var, dt, cta_now_s, cp.value,
                    seed_gap > 0.0 ? readmit_h : 0.0);
                var_eff       = cf.var_eff;
                seed_anchored = cf.anchored;
            }
            if (!cp.inflate_q) {
                const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                   : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                             : 0.0;
                if (floor > 0.0) var_eff = std::max(var_eff, floor);
            }
            var_eff = vce_scale_shared(cp.kind, cp.sat, cp.inflate_q, var_eff);  // M2 v2 factor
            if (!(var_eff > 0.0)) continue;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
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
                est.attribTag("dense_c1");
                est.addSharedPriorDense(dkeys, cov_eff,
                                        anchor_info_trace.is_open() ? &lam : nullptr,
                                        ctx.knobs.carry_gauge_free,
                                        ctx.knobs.carry_gauge_free_closure);
                if (anchor_info_trace.is_open())
                    for (std::size_t a = 0; a < kd; ++a)
                        if (dkeys[a].kind == SharedParamKind::Clock)
                            clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // B7: folded Λ(c,c)
                if (ctx.knobs.carry_gauge_free) {
                    std::size_t nclk = 0;
                    for (std::size_t a = 0; a < kd; ++a)
                        if (dkeys[a].kind == SharedParamKind::Clock) ++nclk;
                    ctx.counters.cta_gf_keys += static_cast<long long>(nclk);
                    if (nclk < 2) ++ctx.counters.cta_gf_inert;
                }
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
        est.attribTag("d2_leftover");
        // §18.12 (invariant #7, silent side): the dense border block is deliberately NOT
        // snapshotted (see the C1 dense-covariance note in networkTryWarmResume), so at
        // the FIRST post-resume fold every carried key misses prev_carry_cov.row and
        // folds through the legacy DIAGONAL path while the restored coupling rows are
        // already conditional.  That asymmetry lasts exactly one epoch and is present in
        // EVERY arm (it is not an A/B confounder), but its SIZE is the follow-on
        // increment's target — measure it instead of assuming it.  Read HERE, one step
        // above the gauge-free / plain addSharedPrior branch below, so the count is the
        // same population on BOTH fold routes.
        if (ctx.carry.resume_cpl_census_pending)
            ctx.counters.n_resume_cpl_diag_border =
                static_cast<long long>(leftovers.size());
        // Card Q3 §2.1: leftovers are the keys the dense fold could not take (no
        // matrix row after a warm resume, or a non-finite screen).  Under the
        // gauge-free fold they form their own group — and a constellation that
        // contributes only ONE leftover clock is entirely gauge, so nothing is
        // folded for it.  That is conservative by construction: the fallback can
        // under-anchor, never re-introduce gauge information.
        if (ctx.knobs.carry_gauge_free)
            ctx.counters.cta_gf_keys +=
                static_cast<long long>(est.addSharedPriorClockGaugeFree(
                    leftovers, ctx.knobs.carry_gauge_free_closure));
        else
            est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), leftovers.begin(), leftovers.end());
    }
    // Card C-GF: the partial-support skip tally is RUN-scoped on the estimator and is
    // read once at teardown — reading it here would miss the withdraw and
    // within-epoch-replay folds, which run after this point in the epoch.

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
        est.attribTag("clock_osc");
        est.addSharedPrior(clkp);
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), clkp.begin(), clkp.end());
    }
}

void networkRxCbDemeanPrepass(NetworkRunContext& ctx,
                              const std::vector<NetStation>& stations,
                              const std::vector<bool>& present,
                              double pl_now_s) {
    if (!ctx.knobs.rx_cb_on || !ctx.knobs.rx_cb_demean
        || !ctx.knobs.rx_cb_demean_within) return;
    const auto& rx_cb_carry = ctx.carry.rx_cb_carry;   // READ-only here
    // Clear the per-epoch trace record FIRST: an epoch that computes no μ (cold
    // start, or a full carry wipe) must not leave the previous epoch's row standing
    // for the trace emitter to publish under this epoch's timestamp.
    ctx.carry.rx_cb_demean_last.clear();
    if (rx_cb_carry.empty()) return;                   // cold start: nothing to average
    constexpr double kRxCbMaxGapS = 900.0;             // the inject's own horizon

    std::map<std::string, double> sum_lam, sum_lam_innov;
    std::map<std::string, int> n_sta;
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (i >= present.size() || !present[i]) continue;   // no data ⇒ no vote
        const NetStation& st = stations[i];
        if (!st.sm) continue;
        const Eigen::VectorXd x_lin = st.sm->getStateVector();
        for (const auto& [cb_key, cb_sidx] : st.sm->getCodeBiasStates()) {
            if (StateKeys::isGlonassPerSlotIfbKey(cb_key)) continue;   // anchored family
            const Eigen::Index cb_idx = static_cast<Eigen::Index>(cb_sidx);
            if (cb_idx < 0 || cb_idx >= x_lin.size()) continue;
            if (!std::isfinite(x_lin(cb_idx))) continue;
            const auto cbit = rx_cb_carry.find({static_cast<int>(i), cb_key});
            if (cbit == rx_cb_carry.end()) continue;           // cold — nothing to fold
            const double gap = pl_now_s - cbit->second.last_seen_s;
            if (gap > kRxCbMaxGapS) continue;                  // the fold would be skipped
            const double var_eff = cbit->second.var
                                 + ctx.knobs.rx_cb_q * std::max(0.0, gap);
            if (!(var_eff > 0.0) || !std::isfinite(var_eff)) continue;
            const double innov = cbit->second.value_abs - x_lin(cb_idx);
            if (!std::isfinite(innov)) continue;
            const double lam = 1.0 / var_eff;
            sum_lam[cb_key]       += lam;
            sum_lam_innov[cb_key] += lam * innov;
            ++n_sta[cb_key];
        }
    }

    // Same reduction, same thin-mean floor, same freeze semantics as the LV-2
    // harvest path — only the EPOCH the result is applied to differs.  Writing
    // ctx.carry.rx_cb_demean_mu here (serially, before PASS B) is what removes the
    // lag; the parallel produce phase reads it exactly as before.
    bool any = false;
    for (const auto& [fam, s] : sum_lam) {
        const auto held = ctx.carry.rx_cb_demean_mu.find(fam);
        const double prev_mu =
            (held != ctx.carry.rx_cb_demean_mu.end()) ? held->second : 0.0;
        const RxCbDemeanReduce red = rxCbDemeanMu(
            sum_lam_innov[fam], s, n_sta[fam], ctx.knobs.rx_cb_demean_min_sta, prev_mu);
        auto& rec = ctx.carry.rx_cb_demean_last[fam];
        rec.sum_lam = s;
        rec.n_sta   = n_sta[fam];
        rec.fresh   = red.fresh;
        rec.mu      = red.mu;
        if (!red.fresh) { ++ctx.carry.lv2_mu_frozen; continue; }
        ctx.carry.rx_cb_demean_mu[fam] = red.mu;
        ctx.carry.lv2_last_mu_m = red.mu;
        ctx.carry.lv2_max_abs_mu_m = std::max(ctx.carry.lv2_max_abs_mu_m,
                                              std::abs(red.mu));
        any = true;
    }
    if (any) {
        ++ctx.carry.lv2_mu_epochs;
        ++ctx.carry.lv5_within_epochs;
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
                                NetworkCarryVce& carry_vce_est,
                                NetworkPriorResidual& prires_est, bool carry_warm,
                                double pl_now_s,
                                const std::vector<std::string>& registered_clock_sats,
                                const std::set<std::string>& shared_phase_keys,
                                const std::vector<std::pair<std::string, std::string>>& registered_code_keys) {
    // Alias block (INC-4 migration mechanic) — the relocated body reads the
    // former runNetwork() locals under their original names.
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& iono_carry = ctx.carry.iono_carry;   // QUEUE 8b
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
    const bool   pl_los_scale = ctx.knobs.pl_los_scale;   // F1: mirror the injector's per-LOS B_eff in the fold-variance readback
    const bool   pl_local_block = ctx.knobs.pl_local_block; // F3: stash the harvest cross-covariance block beside the per-column carry
    const bool   zwd_on = ctx.knobs.zwd_on;
    const bool   iono_on = ctx.knobs.iono_on;             // QUEUE 8b
    const double iono_q = ctx.knobs.iono_q;
    const double iono_var_floor = ctx.knobs.iono_var_floor;
    const double iono_max_gap_s = ctx.knobs.iono_max_gap_s;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // PRIOR-RESIDUAL CENSUS — the pl / zwd /
    // rxcb feed arms.  Resolved once at run start into per-family bools, so an
    // OFF run pays one bool test per harvest loop and nothing else.
    const bool   prires_pl   = ctx.knobs.prires_pl;
    const bool   prires_zwd  = ctx.knobs.prires_zwd;
    const bool   prires_iono = ctx.knobs.prires_iono;   // QUEUE 8b
    const bool   prires_rxcb = ctx.knobs.prires_rxcb;
    const double zwd_q       = ctx.knobs.zwd_q;
    // B9-in-block (network.carry_zwd_in_block): the ZWD column joins the F3 harvest
    // block.  Conjoined with the SAME live predicate the block itself carries
    // (pl_local_block && carry_cpl_cond) so the harvest and the inject agree on which
    // station-epochs have a block at all; carry_cpl_joint is the injector's mutex and
    // never reaches the block path there either.  OFF ⇒ `cols` is untouched and
    // PlHarvestBlock::C keeps its exact pre-card dimension (byte-identical).
    const bool   zwd_blk_on  = ctx.knobs.zwd_in_block && pl_local_block && carry_cpl_cond;
    // I-in-block (network.carry_iono_in_block): the per-LOS iono columns join the SAME
    // harvest block, on the SAME live predicate, for the same reason — so the harvest and
    // the inject agree on which station-epochs have a block at all.  OFF ⇒ `cols` is
    // untouched and PlHarvestBlock::C keeps its exact pre-card dimension (byte-identical).
    const bool   iono_blk_on = ctx.knobs.iono_blk && pl_local_block && carry_cpl_cond;
    const double iono_blk_gap_s = ctx.knobs.iono_blk_max_gap_s;
    // RX-CLK-in-block (network.carry_rx_clock_in_block): the receiver-clock (and ISB)
    // columns join the SAME harvest block, on the SAME live predicate, for the same
    // reason — so the harvest and the inject agree on which station-epochs have a block
    // at all.  OFF ⇒ `cols` is untouched and PlHarvestBlock::C keeps its exact pre-card
    // dimension (byte-identical).
    const bool   rxclk_blk_on  = ctx.knobs.rxclk_blk && pl_local_block && carry_cpl_cond;
    const double rxclk_gap_s   = ctx.knobs.rxclk_max_gap_s;
    const double rxclk_q       = ctx.knobs.rxclk_q_m2;
    const double rxclk_rate_q  = ctx.knobs.rxclk_rate_q;
    const double rxclk_rate_var0 = ctx.knobs.rxclk_rate_var0;
    const double rxclk_jump_k  = ctx.knobs.rxclk_jump_k;
    // Seam-scoped ZWD re-seed + unconditional Z-row capture (§17 finding 3).
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;

    // --- TWO-EDGE CONTINUOUS HANDOVER: ledger harvest -------------------------
    //
    // Record, for every anchor-CANDIDATE column, the station's ABSOLUTE ambiguity
    // estimate in THIS epoch's realized frame, so that if the anchor for that key
    // moves next epoch the election can pin the successor HERE instead of at 0 —
    // making the gauge change an S-transform rather than a re-realization.
    //
    // ⚠ WHY NOT `st.sm->getStateVector()(amb_idx)` AT ELECTION TIME.  That is
    // `x_lin`, a linearization FRAME.  The network solve is never written back into
    // the per-station StateManager (`setStateVector` runs once, at setup), and an
    // `x_lin` shift on a local column is a proven exact solve no-op — so pinning to
    // it would carry no information about where the arc actually sits.  The estimate
    // is `x_lin + back-substituted Δx`, which exists only HERE, after the solve.
    // Hence the one-epoch deferral: harvested now, consumed by the next epoch's
    // election.  Same shape as carry_innov_pending / dw_release_pending /
    // raim_refloat_pending.
    //
    // The population is `R.qc_amb`, the carry-neutral census — columns PRESENT in
    // A_r and UNPINNED.  That is exactly right: a station holding the pin is not a
    // handover TARGET, and its pinned column carries no independent information.
    // Deliberately NOT gated on carry_warm: the whole point is to work on the
    // lineages where the phase-leveling carry does not exist.
    if (ctx.knobs.pd_handover_continuous) {
        ctx.carry.handover.value_abs.clear();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            if (R.qc_amb.empty()) continue;
            const std::size_t est_idx_hv = static_cast<std::size_t>(est_index_of[i]);
            Eigen::VectorXd xr, vhv;
            std::vector<Eigen::Index> cols_hv;
            cols_hv.reserve(R.qc_amb.size());
            for (const auto& pa : R.qc_amb) cols_hv.push_back(pa.col);
            try {
                xr  = est.reconstructStation(est_idx_hv, x_s);
                // The pin is a hard ~1e10-weight constraint, so the re-coupling edge
                // must be able to RE-VALIDATE the value before stamping it in
                // (invariant #7).  BORDER-EXACT flavor, deliberately NOT
                // the leveling carry's border-inclusive harvest: the shared phase-
                // bias column's uncertainty moves the pin target and the realized
                // frame TOGETHER (common-mode in the handover step), and folding it
                // in refused 552/582 settled successors on the 12 h quiet flagship
                // while §5.0's G21 (+17.07 m, 378 σ) sailed under a 1.0 m² ceiling —
                // the marginal gates the wrong quantity in both directions.  What
                // the ceiling must bound is the successor's error RELATIVE to the
                // shared frame, which is what this returns.
                vhv = est.pureConditionalLocalVarianceDiag(est_idx_hv, cols_hv);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < R.qc_amb.size(); ++a) {
                const PlAmb& pa = R.qc_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                const double var = (a < static_cast<std::size_t>(vhv.size()))
                                       ? vhv(static_cast<Eigen::Index>(a)) : -1.0;
                // Key on "sat_signal" — byte-identical to the election's `amb`
                // (kAmbiguityDelimiter is '_'), so the lookup needs no translation.
                AnchorHandoverLedger::Entry hv_e{value_abs, var};
                // network.phase_datum_handover_max_var_ratio: the arc's first-stash
                // conditional variance (0 = never carried ⇒ the ratio test refuses it).
                // Read off LAST epoch's carry (this harvest precedes the leveling harvest).
                {
                    const auto lcit = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    if (lcit != local_carry.end()) hv_e.var_first = lcit->second.var_first;
                }
                ctx.carry.handover.value_abs[{static_cast<int>(i),
                                              pa.sat + "_" + pa.signal}] = hv_e;
            }
        }
        ctx.carry.handover.primed = true;
    }

    // --- Phase-leveling carry: harvest posterior + stash (Increment 1) ----
    // For each present non-pivot station, recover its ambiguities' ABSOLUTE
    // values (x_lin + back-substituted Δx) and their conditional variances
    // (the cheap per-column diagonal harvest, NOT the full per-station inverse)
    // and refresh the persistent carry.  Then drop entries unseen beyond the
    // max gap (a stale-carry / map-growth guard — distinct from the slip-driven
    // REFLOAT/DROP of later increments).
    if (carry_warm) {
        // Build, ONCE per epoch, the shared
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
        // Harvest one carried column's PURE conditional variance (N_rr⁻¹)_cc
        // + its regression on the free shared border (identity-keyed, sparsified).
        // Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
        // F3 (network.phase_leveling_local_block): `cross_cols` / `cross` — when
        // non-null, the SAME per-column solve is also read at the station's other
        // carried columns and handed back as (N_rr⁻¹)_{c c_j} (a gather, no second
        // solve); null on every other call (byte-identical).
        auto harvestCoupling = [&](std::size_t est_idx, Eigen::Index col,
                                   const std::vector<Eigen::Index>* cross_cols,
                                   Eigen::VectorXd* cross)
                -> std::pair<double, std::vector<CoupledShared>> {
            NetworkNormalEquations::StationRegression reg;
            try { reg = est.stationRegression(est_idx, col, /*carry_aware=*/false, cross_cols); }
            catch (...) { return {-1.0, {}}; }
            if (cross) *cross = std::move(reg.cross);
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
                    // A datum-pinned OR ensemble-gauge column is KEPT in the row
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
        // ---- Rung 3 v3 lane observatory: PRE-event snapshot -----------------
        // Taken here because the loop below
        // overwrites local_carry entry-by-entry, so a band's pre-event level is
        // gone the moment its own iteration runs.  Only the freed population is
        // snapshotted, so the OFF path and the no-slip epoch both cost nothing.
        //
        // C(t-) and C(t+) are BOTH read inside this function, hence both in the
        // gauge this epoch's datum work already settled — the frame hazard that
        // killed the seed site cannot arise here.
        // (value_abs, last_seen_s) — the stamp is load-bearing: a carry that was
        // NOT stashed at the immediately preceding epoch spans a different
        // interval from the one-epoch ΔMW it would be differenced against.
        std::map<std::tuple<int, std::string, std::string>, std::pair<double, double>> lane_pre;
        // ⚠ NOT gated on a non-empty lane_mw: an epoch with freed arcs but no raw
        // rows must still reach the measurement loop so each freed LOS lands in
        // n_lane_skip_no_row.  Gating here would restore the silent-denominator
        // defect the no_row channel exists to close.
        const bool lane_obs_on = config_.networkCycleSlipRelevelLaneSnap();
        if (lane_obs_on) {
            for (const auto& fa : ctx.carry.relevel.free_ambs) {
                const auto parts = StateKeys::parseAmbiguityKey(fa.second);
                if (!parts) continue;
                const auto key = std::make_tuple(fa.first, parts->satellite, parts->signal);
                const auto cit = local_carry.find(key);
                if (cit != local_carry.end())
                    lane_pre[key] = { cit->second.value_abs, cit->second.last_seen_s };
                // A freed band's PARTNER may not itself be freed (band-scoped
                // response): snapshot the whole pair, because the partner's
                // carry is what anchors the lane and it is overwritten by the
                // same loop.
                const auto mit = ctx.carry.relevel.lane_mw.find({fa.first, parts->satellite});
                if (mit == ctx.carry.relevel.lane_mw.end()) continue;
                for (const std::string& sig : { mit->second.sig1, mit->second.sig2 }) {
                    if (sig.empty() || sig == parts->signal) continue;
                    const auto pkey = std::make_tuple(fa.first, parts->satellite, sig);
                    if (lane_pre.count(pkey)) continue;
                    const auto pit = local_carry.find(pkey);
                    if (pit != local_carry.end())
                        lane_pre[pkey] = { pit->second.value_abs, pit->second.last_seen_s };
                }
            }
        }

        // B9-in-block (network.carry_zwd_in_block): the ZWD harvest is a SEPARATE loop
        // further down (it must run for stations with no carried ambiguities at all), so
        // the block this loop allocates — and the exact `cols` order its rows are indexed
        // by — are handed forward per station.  Empty vectors when the knob is off, and
        // nothing below touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> zblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       zblk_cols_of_sta;
        if (zwd_blk_on) {
            zblk_of_sta.resize(stations.size());
            zblk_cols_of_sta.resize(stations.size());
        }
        // I-in-block: the same hand-forward for the per-LOS iono harvest loop below.
        // `iblk_row0_of_sta` is the block index of this station's FIRST iono column —
        // the iono columns are appended immediately after the ambiguities and BEFORE the
        // ZWD, so the ZWD keeps the last-row slot its own (validated) card computes as
        // C.rows()−1.  Empty vectors when the knob is off; nothing below touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> iblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       iblk_cols_of_sta;
        std::vector<Eigen::Index>                    iblk_row0_of_sta;
        if (iono_blk_on) {
            iblk_of_sta.resize(stations.size());
            iblk_cols_of_sta.resize(stations.size());
            iblk_row0_of_sta.assign(stations.size(), -1);
        }
        // RX-CLK-in-block: the same hand-forward for the receiver-clock harvest loop.
        // `rblk_row0_of_sta` is the block index of this station's FIRST clock-family
        // column — appended after the iono columns and BEFORE the ZWD, so the ZWD keeps
        // the last-row slot its own (validated) card computes as C.rows()−1.
        std::vector<std::shared_ptr<PlHarvestBlock>> rblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       rblk_cols_of_sta;
        std::vector<Eigen::Index>                    rblk_row0_of_sta;
        if (rxclk_blk_on) {
            rblk_of_sta.resize(stations.size());
            rblk_cols_of_sta.resize(stations.size());
            rblk_row0_of_sta.assign(stations.size(), -1);
        }
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            // Prioritized: harvest the former pivot too — its non-anchored arcs
            // are in R.pl_amb; anchored (pinned) keys were excluded at build, so
            // inject and harvest partition on the SAME per-(sat,signal) predicate.
            if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
            const StationAssembly& R = asm_res[i];
            // I-in-block: a station with NO carry-eligible ambiguities can still have
            // live per-LOS iono columns, and PASS B builds a block for exactly that case
            // (its `if (!pl_pend.empty() || !iono_bc.empty())` gate).  Skipping it here
            // would leave those entries stashed WITHOUT a block, so every pair with them
            // would be zeroed next epoch — a silently diagonal iono prior, i.e. the very
            // vehicle this card replaces.  Harvest and inject must agree on which
            // station-epochs have a block.  OFF ⇒ iono_blk_on is false and this is
            // exactly the pre-card `if (R.pl_amb.empty()) continue;` (byte-identical).
            // RX-CLK-in-block: and the same for a station whose only live carry is its
            // receiver clock — PASS B builds a block for that case too, and skipping it
            // here would stash the entry WITHOUT a block, i.e. silently diagonalize the
            // most station-common direction of all.
            if (R.pl_amb.empty() && !(iono_blk_on && !R.pl_iono.empty())
                && !(rxclk_blk_on && !R.pl_rxclk.empty())) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_amb.size() + 1 + R.pl_iono.size() + R.pl_rxclk.size());
            for (const auto& a : R.pl_amb) cols.push_back(a.col);
            // I-in-block: the station's present per-LOS iono columns become block indices
            // n_amb … n_amb+k−1, appended BEFORE the ZWD so the ZWD's own card keeps its
            // "the appended column is the LAST row" invariant.  Every per-column solve
            // below then gathers (N_rr⁻¹)_{c_a, icol} for free, the same gather that
            // already pays for the ambiguity–ambiguity terms.  R.pl_iono is PASS B's
            // record of what it pushed, so a station whose block was never built keeps
            // the pre-card `cols`.
            const Eigen::Index iono_row0 = static_cast<Eigen::Index>(cols.size());
            const bool iono_in_cols = iono_blk_on && !R.pl_iono.empty();
            if (iono_in_cols)
                for (const auto& t : R.pl_iono) cols.push_back(std::get<1>(t));
            // B9-in-block: the station's ZWD column becomes block index n (the LAST
            // entry), so every per-column solve below gathers its (N_rr⁻¹)_{c_a, zcol}
            // cross term for free — the same gather that already pays for the
            // ambiguity-ambiguity terms.  Appended AFTER the ambiguity columns so the
            // pl rows keep their exact indices `a`; the ZWD's own row n is written by
            // the ZWD loop below.  R.pl_zwd_present is PASS B's record, so a station
            // whose tropo column was absent this epoch simply keeps the pre-card `cols`.
            // RX-CLK-in-block: the station's receiver-clock (and ISB) columns become the
            // next block indices, appended AFTER the iono columns and BEFORE the ZWD so the
            // ZWD's own card keeps its "the appended column is the LAST row" invariant.
            // Every per-column solve below then gathers (N_rr⁻¹)_{c_a, rcol} for free.
            const Eigen::Index rxclk_row0 = static_cast<Eigen::Index>(cols.size());
            const bool rxclk_in_cols = rxclk_blk_on && !R.pl_rxclk.empty();
            if (rxclk_in_cols)
                for (const auto& t : R.pl_rxclk) cols.push_back(t.col);
            const bool zwd_in_cols = zwd_blk_on && R.pl_zwd_present
                                     && R.pl_zwd.col >= 0;
            if (zwd_in_cols) cols.push_back(R.pl_zwd.col);
            Eigen::VectorXd xr, vard;
            try {
                xr = est.reconstructStation(est_idx, x_s);
                if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc, not this MARGINAL
                    vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            // F3 (network.phase_leveling_local_block): this station-epoch's harvest
            // CROSS-covariance block C(a,b) = (N_rr⁻¹)_{c_a c_b} over R.pl_amb's columns
            // — one shared_ptr per station-epoch, filled row by row below from each
            // column's own n_c solve (the gather stationRegression already pays for),
            // and pointed at by every entry stashed at THIS harvest.  Re-stashed WHOLE
            // every epoch from the posterior, so Σ never drifts across epochs; the
            // previous block is released as its last referencing entry is replaced.
            // Null when the knob is off ⇒ nothing below allocates or writes.
            std::shared_ptr<PlHarvestBlock> hblk;
            if (pl_local_block && carry_cpl_cond) {
                hblk = std::make_shared<PlHarvestBlock>();
                const Eigen::Index nb = static_cast<Eigen::Index>(cols.size());
                hblk->C = Eigen::MatrixXd::Constant(nb, nb, std::numeric_limits<double>::quiet_NaN());
                hblk->stamp_s = pl_now_s;
                // B9-in-block: hand the block + its column order to the ZWD loop below,
                // which owns row n.  Handed over ONLY when the ZWD is actually in `cols`
                // — otherwise the ZWD loop must not write a row that does not exist.
                if (zwd_in_cols) {
                    zblk_of_sta[i]      = hblk;
                    zblk_cols_of_sta[i] = cols;
                }
                // I-in-block: same hand-forward, plus the row offset the iono loop needs.
                if (iono_in_cols) {
                    iblk_of_sta[i]      = hblk;
                    iblk_cols_of_sta[i] = cols;
                    iblk_row0_of_sta[i] = iono_row0;
                }
                // RX-CLK-in-block: same hand-forward, plus the row offset its loop needs.
                if (rxclk_in_cols) {
                    rblk_of_sta[i]      = hblk;
                    rblk_cols_of_sta[i] = cols;
                    rblk_row0_of_sta[i] = rxclk_row0;
                }
            }
            for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                const PlAmb& pa = R.pl_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                Eigen::VectorXd cross;   // F3: (N_rr⁻¹)_{c_a c_b} over the station's carried columns (empty OFF)
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, pa.col, hblk ? &cols : nullptr,
                                              hblk ? &cross : nullptr);
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
                // PRIOR-RESIDUAL CENSUS:
                // the SAME (prior − solved, gap, post_var, fold_var) tuple the
                // VCE monitor folds, but WITHOUT its two exclusions — those become
                // the RESPONDED / FRAME labels, which is exactly the population a
                // detector exists to find and a variance component is right to
                // drop.  Hoisted above the monitor so both read one qualification.
                //
                // `folded` is REPRODUCED from the inject predicate, not mirrored by
                // hand: every clause of the PASS-B injector is a pure function of
                // values still in scope here — the carry entry, the gap, the
                // pending-reseed mark (cleared only after this point), pa.x_lin and
                // var_eff.  Getting it wrong is not cosmetic: the folded branch on
                // an unfolded row inflates |t| ~3000× through the ε clamp and would
                // make the positive control pass vacuously.  Two clauses are not
                // reproducible here and are assumed to have held, since the ambiguity
                // reached R.pl_amb at all: the gauge-pin exclusion and the h2local
                // column lookup.  Note eq34_reref is a FRAME LABEL for pl, not a
                // folded=false condition — the pl injector has no eq34 skip, so a
                // re-referenced arc really did fold.  Truth wins over symmetry.
                const bool prires_want = prires_pl && prires_est.enabled();
                if (carry_vce || prires_want) {
                    const auto oc = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    const bool reseed_pending =
                        stations[i].sm->isAmbiguityPendingReseed(pa.amb_key);
                    const bool reref = eq34_reref.count({pa.sat, pa.signal}) > 0;
                    // SPEC_slip_relevel_walk harvest guard: a freed or bridged arc's
                    // (prior − solved) innovation is slip-scale — without this it
                    // would be ingested as a CLEAN row (the detector's own mark was
                    // consumed by the PASS-B pre-pass, so reseed_pending is false
                    // here for every detector-caught slip).
                    const bool relevel_freed =
                        ctx.carry.relevel.free_ambs.count({static_cast<int>(i), pa.amb_key}) > 0;
                    const bool relevel_bridged =
                        ctx.carry.relevel.bridged.count(
                            {static_cast<int>(i), pa.sat, pa.signal}) > 0;
                    const bool relevel_retapped =
                        ctx.carry.relevel_retapped_prev.count(
                            {static_cast<int>(i), pa.sat, pa.signal}) > 0;
                    if (oc != local_carry.end()) {
                        const double gapv = pl_now_s - oc->second.last_seen_s;
                        if (gapv >= 0.0 && gapv <= pl_max_gap_s) {
                            const double fold_var_hand =
                                [&] {
                                    double v = oc->second.var + pl_q_amb * std::max(0.0, gapv);
                                    // F1 mirror: the injector multiplied THIS column's
                                    // variance by its LOS B_eff (before the floor).  Keyed
                                    // by the injected (sat, signal), so a sibling band the
                                    // inject skipped (freed / stale / reseed) is NOT scaled
                                    // here either — the mirror reports the weight the fold
                                    // ACTUALLY used, or the hand value when it did not fold.
                                    // F3 (phase_leveling_local_block): a column carried in
                                    // a whitened block folded with the block's JOINT Σ,
                                    // whose diagonal for this column is exactly this hand
                                    // value (+ the Card-1d / departed terms neither path
                                    // mirrors) — so the mirror reports Σ's DIAGONAL, the
                                    // column's MARGINAL prior variance, unscaled.  That is
                                    // a decision, not an omission: the census standardizes
                                    // e by the marginal prior σ the column actually carried,
                                    // and pl_los_beff is filled ONLY on the F1 route (under
                                    // F3, a factorization fallback), so the ×B_eff below is
                                    // automatically absent for block columns.
                                    if (pl_los_scale) {
                                        const auto bf = R.pl_los_beff.find({pa.sat, pa.signal});
                                        if (bf != R.pl_los_beff.end() && bf->second > 1)
                                            v *= static_cast<double>(bf->second);
                                    }
                                    if (pl_var_floor > 0.0) v = std::max(v, pl_var_floor);
                                    return v;
                                }();
                            // M2 v2: the PASS-B injector
                            // multiplies the SAME hand value by the SAME α² (the factor
                            // table is refreshed once per epoch, at the end, so inject and
                            // harvest read one value), so `fold_var` here is the variance
                            // the fold ACTUALLY used.  Both consumers want that: the census
                            // standardizes by the used weight, and the monitor's
                            // sigma_hat_post readout is only "1 at convergence" if its
                            // denominator is the used weight too.  The HAND value goes in
                            // alongside it because it is the factor iteration's own
                            // denominator and must stay α²-independent.  Unity when OFF.
                            const double fold_var =
                                fold_var_hand * ctx.carry.vce_apply.factor(
                                                    NetworkCarryVceApply::kPl, pa.sat);
                            // ⚠ THE CENSUS NUMERATOR IS A SEPARATE, OPEN CARD.
                            // `innov` is the raw carried-minus-solved difference.
                            // Under the Card-1 conditional coupling the carried
                            // prior's RHS is not `value_abs` alone — the coupling
                            // rows contribute Σ M_rj·ŝ_prev,j to it
                            // (NetworkCoupling.cpp:238, the `rhs += cs.coeff *
                            // cs.s_prev` accumulation) — so on a coupled posture
                            // this ê and the ê the fold actually saw differ by the
                            // regressor term.  The DENOMINATOR side of that same
                            // mismatch is what produces the r_negative population
                            // (post_var > fold_var: the harvested posterior is a
                            // CONDITIONAL variance), which the census now
                            // re-partitions into its own RNEG class rather than
                            // letting it head the ladder.  Aligning the numerator
                            // is deliberately NOT part of that change: it would
                            // move every coupled family's ê and therefore its
                            // σ̂₀, i.e. it is solve-adjacent measurement work with
                            // its own A/B, not a readout re-partition.
                            const double innov = oc->second.value_abs - value_abs;
                            if (carry_vce && !reseed_pending && !reref
                                && !relevel_freed && !relevel_bridged
                                && !relevel_retapped)
                                carry_vce_est.addRow("pl", pa.sat, innov,
                                                     gapv, var, fold_var, fold_var_hand);
                            if (prires_want) {
                                NetworkPriorResidual::Input in;
                                in.family   = "pl";
                                in.sat      = pa.sat;
                                in.signal   = pa.signal;
                                in.station  = static_cast<int>(i);
                                in.e        = innov;
                                in.fold_var = fold_var;
                                in.post_var = var;
                                in.gap_s    = gapv;
                                // The inject predicate, reproduced (see above).
                                // A freed arc's inject was SKIPPED (the latched set);
                                // a bridged arc's (shifted) prior DID fold.
                                in.folded   = !reseed_pending && !relevel_freed
                                           && std::isfinite(pa.x_lin)
                                           && fold_var > 0.0;
                                in.label = (reseed_pending || relevel_freed || relevel_bridged
                                            || relevel_retapped)
                                               ? PriorResidualLabel::Responded
                                         : reref ? PriorResidualLabel::Frame
                                                 : PriorResidualLabel::Clean;
                                in.mech  = reseed_pending   ? "reseed"
                                         : relevel_bridged  ? "delta_bridge"
                                         : relevel_retapped ? "raim_retap"
                                         : relevel_freed    ? "relevel_free"
                                         : reref            ? "eq34_reref" : "";
                                prires_est.addRow(in);
                            }
                        }
                    }
                }
                // §18.12 OBSERVATORY (the SPEC_lane_slip_response §7.3 measure-first
                // shape): at the FIRST post-resume harvest the restored regressor and
                // this epoch's freshly derived one are both in hand.  Their agreement is
                // the ONLY direct evidence the restored row was frame-valid.  Read it
                // against the algebra: a row with A_r = e_c, A_s = M contributes
                // α(M − M_d)(M − M_d)ᵀ to the reduced SHARED normals and α(y − β)(M − M_d)
                // to the shared RHS, so M == M_d makes the row EXACTLY shared-neutral and
                // rel ≪ 1 proves it was.  M == 0 — today's diagonal restore — is the
                // MAXIMAL deviation, and that drive is the 4-8 cm the 9.5 mm clock priors
                // lose to.  Nothing is written back: byte-identical on every solve artifact.
                if (ctx.carry.resume_cpl_census_pending && carry_cpl_cond
                    && !coupling.empty()) {
                    const auto old = local_carry.find(std::make_tuple(
                        static_cast<int>(i), pa.sat, pa.signal));
                    if (old != local_carry.end() && !old->second.coupling.empty()) {
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
                        for (const CoupledShared& cs : old->second.coupling) {
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
                }
                {
                    // First-stash conditional variance: set once at the arc's birth, kept
                    // across re-stashes (the honest-σ-scale denominator of the handover
                    // ratio ceiling; an erase-and-rebirth starts it over, a re-level keeps
                    // it so a refloated arc reads unconverged until it re-earns the level).
                    const auto lkey = std::make_tuple(static_cast<int>(i), pa.sat, pa.signal);
                    const auto oldc = local_carry.find(lkey);
                    const double var_first =
                        (oldc != local_carry.end() && oldc->second.var_first > 0.0)
                            ? oldc->second.var_first : var;
                    // The arc's BIRTH — birth_ep (the F3 block admission age gate's
                    // and the candidacy bar's clock) and its seconds twin birth_s (the
                    // sim-only slip oracle's hard window floor: a truth slip stamped at
                    // or before the entry's creation is already absorbed by the level
                    // the harvest CREATED and must never be re-applied) — is stamped by
                    // localCarryStampBirth below, the ONE definition of the
                    // first-stash-wins rule (filter/NetworkRunTypes.hpp).  First stash
                    // wins, kept across re-stashes, restarted by an erase-and-rebirth
                    // (which needs no clause: the entry is GONE, so `oldc == end()` and
                    // today's epoch is the honest stamp).
                    // ⚠ Its "already stamped" test is `!= 0`, NOT `> 0`
                    // (SPEC_event_harness §15.4): a warm-resume-restored entry carries
                    // LocalCarry::kBirthEpRestored / kBirthSRestored precisely so the
                    // rule can tell "restored, true age not snapshotted ⇒ MATURE" from
                    // "never stashed ⇒ stamp it now".  With `> 0` the FIRST post-resume
                    // harvest re-branded every restored arc as born at the resume epoch,
                    // and every age gate then saw a network of newborns for min_age
                    // epochs (390 of 414 candidacy skips read `young` on a 60-epoch leg).
                    // AGE-AWARE young-arc inflate stamp (SPEC_amb_iono_split_overcount
                    // §6.17–§6.18).  Two events, and ONLY two, produce a level whose
                    // VALUE is code-quality while its harvested VARIANCE is phase-stiff:
                    //   * the entry is CREATED — a first stash, or the re-stash after a
                    //     T4 attribution erase / gap prune DELETED the entry (the erase
                    //     path needs no separate clause precisely because it deletes);
                    //   * the arc was FREED this epoch, so THIS harvest re-levelled it
                    //     from one epoch of code.
                    // The freed predicate is the harvest's own `relevel_freed` above,
                    // re-read here because that one lives inside the VCE/census scope:
                    // `relevel.free_ambs` is the LATCHED set the PASS-B inject consulted
                    // (empty when network.cycle_slip.relevel_free_epoch is off, which is
                    // why no knob guard is needed — the same rule the census uses).
                    // Every OTHER re-stash PRESERVES the stamp, so the age keeps growing
                    // while the arc matures.  Deliberately NOT birth_s: a re-level keeps
                    // birth_s (by design — the slip oracle's floor), and it is exactly
                    // the re-level, not the birth, that resets what the level is worth.
                    const bool yng_created = (oldc == local_carry.end());
                    const bool yng_freed =
                        ctx.carry.relevel.free_ambs.count(
                            {static_cast<int>(i), pa.amb_key}) > 0;
                    LocalCarry nlc{value_abs, var, pl_now_s, std::move(coupling)};
                    nlc.var_first = var_first;
                    localCarryStampBirth(
                        nlc, (oldc != local_carry.end() ? &oldc->second : nullptr),
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    nlc.relevel_s = NetworkYoungInflate::stamp(
                        yng_created ? 0.0 : oldc->second.relevel_s,
                        yng_created, yng_freed, pl_now_s);
                    if (yng_created)     ++ctx.counters.n_yng_stamp_create;
                    else if (yng_freed)  ++ctx.counters.n_yng_stamp_free;
                    local_carry[lkey] = std::move(nlc);
                }
                if (hblk && cross.size() == hblk->C.rows()) {
                    // F3: row a of the station's harvest block = this column's n_c read
                    // at every carried column (its own entry equals `var`); the entry
                    // points at the block + its index so the next inject can pair it
                    // with any sibling stashed at THIS harvest — and with nothing else.
                    hblk->C.row(static_cast<Eigen::Index>(a)) = cross.transpose();
                    auto& ent = local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)];
                    ent.blk = hblk;
                    ent.blk_idx = static_cast<int>(a);
                }
                // One-shot IF-phase refloat: the fresh post-slip value is now
                // stashed, so drop any pending-reseed mark and let the next epoch
                // resume normal carry on the new arc.  Cleared only after a
                // SUCCESSFUL stash — a skipped non-finite value keeps the mark so
                // the stale pre-slip carry is never re-trusted.  No-op when unmarked.
                stations[i].sm->clearPendingReseed(pa.amb_key);
            }
        }
        // ---- Rung 3 v3 lane observatory: POST-event measurement -------------
        // Runs after the harvest loop, so both
        // bands' fresh levels coexist, and before the gap prune, so a pair that
        // is about to be time-pruned is still measurable this epoch.
        //
        //   delta = [C_a(t+)/lam_a - C_b(t+)/lam_b]
        //         - [C_a(t-)/lam_a - C_b(t-)/lam_b] - dM        [cycles]
        //
        // Every per-(station, satellite, pair) bias — receiver and satellite,
        // code and phase — is constant across the event and cancels in BOTH
        // differences, so no bias convention has to be reconciled (the v1 kill)
        // and snapped/unsnapped arcs would mix freely in a DD (the v1 DD-closure
        // kill).  MEASURE ONLY: nothing is written back.
        if (lane_obs_on) {
            // Denominator is the FREED set, not the raw rows: a freed arc the raw
            // pass never saw must show up as a counted skip, not as a silent
            // shrinkage of the denominator (invariant #3 - measure the deferral).
            std::set<std::pair<int, std::string>> freed_los;
            // §7.6(c): LOS-level freeing-cause union (a satellite freed on two
            // bands by two different detectors carries both bits).
            std::map<std::pair<int, std::string>, std::uint8_t> los_cause;
            for (const auto& fa : ctx.carry.relevel.free_ambs) {
                const auto parts = StateKeys::parseAmbiguityKey(fa.second);
                if (!parts) continue;
                const std::pair<int, std::string> lk{fa.first, parts->satellite};
                freed_los.insert(lk);
                const auto cit2 = ctx.carry.relevel.free_cause.find(fa);
                if (cit2 != ctx.carry.relevel.free_cause.end())
                    los_cause[lk] |= cit2->second;
            }
            // WARNING - SIGNAL-CODE NAMESPACE.  The raw pass names a band by the
            // lexicographically first attribute it tracks (L1C), while the
            // ambiguity / carry key uses the ESTIMATOR-selected code (L1W under a
            // "W-before-C" priority list).  Matching the two by full code would
            // miss every entry on such a network and report the miss as "no
            // pre-event carry" - a false population verdict, not a wiring alarm.
            // Resolve by BAND DIGIT against the carry itself, which is the
            // namespace both lane levels are read from.
            const auto resolve = [&](int sta, const std::string& sat,
                                     const std::string& code) -> std::string {
                if (code.size() < 2) return {};
                auto it = local_carry.lower_bound(std::make_tuple(sta, sat, std::string{}));
                for (; it != local_carry.end(); ++it) {
                    if (std::get<0>(it->first) != sta || std::get<1>(it->first) != sat) break;
                    const std::string& sig = std::get<2>(it->first);
                    if (sig.size() >= 2 && sig[1] == code[1]) return sig;
                }
                return {};
            };
            for (const auto& los : freed_los) {
                const int  sta = los.first;
                const std::string& sat = los.second;
                ++ctx.counters.n_lane_attempts;
                // §7.6(a): one per-event row per ATTEMPT — the skip channel is
                // the outcome, so the trace closes against the counters.
                RelevelEpochState::LaneObsRow row;
                row.sta = sta; row.sat = sat;
                {
                    const auto lc = los_cause.find(los);
                    if (lc != los_cause.end()) row.cause = lc->second;
                }
                const auto skip = [&](long long& counter, const char* outcome) {
                    ++counter;
                    row.outcome = outcome;
                    ctx.carry.relevel.lane_obs_rows.push_back(row);
                };
                const auto mit = ctx.carry.relevel.lane_mw.find(los);
                if (mit == ctx.carry.relevel.lane_mw.end()) {
                    skip(ctx.counters.n_lane_skip_no_row, "skip_no_row"); continue;
                }
                const auto& m = mit->second;
                row.sig1 = m.sig1; row.sig2 = m.sig2;
                row.el_deg = m.el_deg;
                row.d_mw_cyc = m.measured ? m.d_mw_cyc : 0.0;
                if (m.sig2.empty())    { skip(ctx.counters.n_lane_skip_no_mw,        "skip_no_mw");        continue; }
                if (!m.measured)       { skip(ctx.counters.n_lane_skip_unmeasured,   "skip_unmeasured");   continue; }
                if (m.pair_changed)    { skip(ctx.counters.n_lane_skip_pair_changed, "skip_pair_changed"); continue; }
                if (!(m.lam1_m > 0.0) || !(m.lam2_m > 0.0)
                    || !std::isfinite(m.lam1_m) || !std::isfinite(m.lam2_m)
                    || !(m.lam2_m > m.lam1_m)) {   // see the PASS-A note on lambda_wl's sign
                    skip(ctx.counters.n_lane_skip_bad_lambda, "skip_bad_lambda"); continue;
                }
                const std::string sa = resolve(sta, sat, m.sig1);
                const std::string sb = resolve(sta, sat, m.sig2);
                if (sa.empty() || sb.empty()) { skip(ctx.counters.n_lane_skip_no_pre, "skip_no_pre"); continue; }
                const auto ka = std::make_tuple(sta, sat, sa);
                const auto kb = std::make_tuple(sta, sat, sb);
                // The RESPONSE must have touched this pair.  A satellite can be in
                // freed_los because a THIRD band was freed (L5 on a triple posture,
                // or a second channel of the same band under channel_mode=all); both
                // of THIS pair's carries are then continuous, there is no re-level to
                // measure, and admitting it would fold ordinary carried-prior
                // evolution into the histogram - biasing it toward the pre-registered
                // "mass below ~2 cyc" conclusion.
                const bool a_freed =
                    ctx.carry.relevel.free_ambs.count({sta, StateKeys::makeAmbiguityKey(sat, sa)}) > 0;
                const bool b_freed =
                    ctx.carry.relevel.free_ambs.count({sta, StateKeys::makeAmbiguityKey(sat, sb)}) > 0;
                if (!a_freed && !b_freed) {
                    skip(ctx.counters.n_lane_skip_no_freed_band, "skip_no_freed_band"); continue;
                }
                // A delta-bridged band's carry was already re-based to
                // pre_slip + shift, so its lane response is the BRIDGE's, not the
                // free re-level's - a different population, counted apart.
                if (ctx.carry.relevel.bridged.count(ka)
                    || ctx.carry.relevel.bridged.count(kb)) {
                    skip(ctx.counters.n_lane_skip_bridged, "skip_bridged"); continue;
                }
                const auto pa_it = lane_pre.find(ka), pb_it = lane_pre.find(kb);
                if (pa_it == lane_pre.end() || pb_it == lane_pre.end()) {
                    skip(ctx.counters.n_lane_skip_no_pre, "skip_no_pre"); continue;
                }
                // PRE freshness: exactly one processed epoch old.  Otherwise the
                // carry difference spans a different interval from the one-epoch
                // MW step it is differenced against.
                if (!(ctx.carry.pl_prev_now_s > 0.0)
                    || pa_it->second.second != ctx.carry.pl_prev_now_s
                    || pb_it->second.second != ctx.carry.pl_prev_now_s) {
                    skip(ctx.counters.n_lane_skip_stale_pre, "skip_stale_pre"); continue;
                }
                // POST freshness: RE-STASHED this epoch, tested on last_seen_s and
                // NOT on map presence.  An un-refreshed entry survives a full
                // pl_max_gap_s, and accepting it would contribute the PRE value as
                // the POST value - collapsing the delta either to a one-band code
                // re-level (top bins) or to exactly -dMW (~0.35 cyc, bottom bins).
                // Both directions manufacture a verdict, in opposite senses.
                const auto qa_it = local_carry.find(ka), qb_it = local_carry.find(kb);
                if (qa_it == local_carry.end() || qb_it == local_carry.end()
                    || qa_it->second.last_seen_s != pl_now_s
                    || qb_it->second.last_seen_s != pl_now_s) {
                    skip(ctx.counters.n_lane_skip_no_post, "skip_no_post"); continue;
                }
                const double pre_lane  = pa_it->second.first / m.lam1_m
                                       - pb_it->second.first / m.lam2_m;
                const double post_lane = qa_it->second.value_abs / m.lam1_m
                                       - qb_it->second.value_abs / m.lam2_m;
                const double delta = post_lane - pre_lane - m.d_mw_cyc;
                if (!std::isfinite(delta)) { skip(ctx.counters.n_lane_skip_bad_lambda, "skip_bad_lambda"); continue; }
                ++ctx.counters.n_lane_measured;
                const bool vertex_wide = a_freed && b_freed;
                ++(vertex_wide ? ctx.counters.n_lane_both_freed
                               : ctx.counters.n_lane_one_freed);
                // §7.6(c): cause split over MEASURED pairs (bit counts).
                if (row.cause & RelevelEpochState::kCauseMw)   ++ctx.counters.n_lane_meas_mw;
                if (row.cause & RelevelEpochState::kCauseLli)  ++ctx.counters.n_lane_meas_lli;
                if (row.cause & RelevelEpochState::kCauseIfTd) ++ctx.counters.n_lane_meas_iftd;
                const double ad = std::abs(delta);
                ctx.counters.lane_abs_delta_sum_cyc += ad;
                if (ad > ctx.counters.lane_abs_delta_max_cyc)
                    ctx.counters.lane_abs_delta_max_cyc = ad;
                static const double kBins[8] = { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0 };
                int bin = 8;
                for (int bi = 0; bi < 8; ++bi) if (ad < kBins[bi]) { bin = bi; break; }
                ++ctx.counters.n_lane_hist[bin];
                // §7.6(b): the band-scoped subset separately, so the rung-2
                // contrast (vertex-wide vs band-scoped response) is readable.
                if (!vertex_wide) ++ctx.counters.n_lane_hist_bandscoped[bin];
                row.measured = true;
                row.vertex_wide = vertex_wide;
                row.delta_cyc = delta;
                ctx.carry.relevel.lane_obs_rows.push_back(row);
            }
            // ---- §7.6(d): T4-side measurement over LAST epoch's pre-erase
            // captures (the RAIM attribution/consensus erase channel — 97.3 %
            // of all field freeing, structurally invisible to free_ambs).  The
            // SAME delta, one extra interlock: the capture is a COPY across an
            // epoch boundary, so a pair whose (sat,σ) was eq34-re-referenced
            // THIS epoch is skipped (the detector path gets that guard for
            // free because lane_pre reads local_carry, which the re-reference
            // hygiene already erased).  Measure-only; separate closure.
            static const double kT4Bins[8] = { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0 };
            if (!ctx.carry.lane_t4_pre.empty()) {
                std::set<std::pair<int, std::string>> t4_los;
                for (const auto& [tk, tv] : ctx.carry.lane_t4_pre)
                    t4_los.insert({std::get<0>(tk), std::get<1>(tk)});
                for (const auto& los : t4_los) {
                    const int  sta = los.first;
                    const std::string& sat = los.second;
                    ++ctx.counters.n_lane_t4_attempts;
                    RelevelEpochState::LaneObsRow row;
                    row.sta = sta; row.sat = sat;
                    row.t4 = true; row.vertex_wide = true;
                    const auto t4_lo = ctx.carry.lane_t4_pre.lower_bound(
                        std::make_tuple(sta, sat, std::string{}));
                    const auto t4_end = ctx.carry.lane_t4_pre.end();
                    const auto t4_same = [&](decltype(t4_lo) it2) {
                        return it2 != t4_end && std::get<0>(it2->first) == sta
                            && std::get<1>(it2->first) == sat;
                    };
                    for (auto it2 = t4_lo; t4_same(it2); ++it2)
                        row.cause |= it2->second.cause;
                    const auto skip = [&](long long& counter, const char* outcome) {
                        ++counter;
                        row.outcome = outcome;
                        ctx.carry.relevel.lane_obs_rows.push_back(row);
                    };
                    const auto mit = ctx.carry.relevel.lane_mw.find(los);
                    if (mit == ctx.carry.relevel.lane_mw.end()) {
                        skip(ctx.counters.n_lane_t4_skip_no_row, "t4_skip_no_row"); continue;
                    }
                    const auto& m = mit->second;
                    row.sig1 = m.sig1; row.sig2 = m.sig2;
                    row.el_deg = m.el_deg;
                    row.d_mw_cyc = m.measured ? m.d_mw_cyc : 0.0;
                    if (m.sig2.empty())    { skip(ctx.counters.n_lane_t4_skip_no_mw,        "t4_skip_no_mw");        continue; }
                    if (!m.measured)       { skip(ctx.counters.n_lane_t4_skip_unmeasured,   "t4_skip_unmeasured");   continue; }
                    if (m.pair_changed)    { skip(ctx.counters.n_lane_t4_skip_pair_changed, "t4_skip_pair_changed"); continue; }
                    if (!(m.lam1_m > 0.0) || !(m.lam2_m > 0.0)
                        || !std::isfinite(m.lam1_m) || !std::isfinite(m.lam2_m)
                        || !(m.lam2_m > m.lam1_m)) {
                        skip(ctx.counters.n_lane_t4_skip_bad_lambda, "t4_skip_bad_lambda"); continue;
                    }
                    // Resolve the captured bands by BAND DIGIT against the raw
                    // pair (the capture keys carry the estimator-selected carry
                    // signal names — the same namespace note as `resolve`).
                    const auto cap_of = [&](const std::string& code)
                        -> std::pair<const std::string*, const NetworkCarryState::LaneT4Pre*> {
                        if (code.size() < 2) return {nullptr, nullptr};
                        for (auto it2 = t4_lo; t4_same(it2); ++it2) {
                            const std::string& sig = std::get<2>(it2->first);
                            if (sig.size() >= 2 && sig[1] == code[1])
                                return {&std::get<2>(it2->first), &it2->second};
                        }
                        return {nullptr, nullptr};
                    };
                    const auto ca = cap_of(m.sig1);
                    const auto cb = cap_of(m.sig2);
                    if (!ca.second || !cb.second) {
                        skip(ctx.counters.n_lane_t4_skip_no_pre, "t4_skip_no_pre"); continue;
                    }
                    // §7.7 handover cross-guard: EVERY frame channel that can
                    // re-realize this pair's family between capture and
                    // measurement — the eq34 (sat,σ) re-reference (this
                    // lineage), the phase-anchor election's re-referenced
                    // families (non-eq34 lineages; the set deliberately
                    // persists across withheld epochs until a stash commits
                    // the new frame), and the datum-station slip's
                    // network-wide reseed (per-satellite, all signals).
                    if (eq34_reref.count({sat, *ca.first})
                        || eq34_reref.count({sat, *cb.first})
                        || ctx.carry.phase_anchor_reref.count({sat, *ca.first})
                        || ctx.carry.phase_anchor_reref.count({sat, *cb.first})
                        || ctx.carry.relevel.datum_reref_sats.count(sat)) {
                        skip(ctx.counters.n_lane_t4_skip_gauge, "t4_skip_gauge"); continue;
                    }
                    if (!(ctx.carry.pl_prev_now_s > 0.0)
                        || ca.second->stamp_s != ctx.carry.pl_prev_now_s
                        || cb.second->stamp_s != ctx.carry.pl_prev_now_s) {
                        skip(ctx.counters.n_lane_t4_skip_stale_pre, "t4_skip_stale_pre"); continue;
                    }
                    // POST: re-stashed THIS epoch under the SAME signal names —
                    // an attribute re-selection after the refloat is a different
                    // lane, not a measurable response.
                    const auto qa_it = local_carry.find(std::make_tuple(sta, sat, *ca.first));
                    const auto qb_it = local_carry.find(std::make_tuple(sta, sat, *cb.first));
                    if (qa_it == local_carry.end() || qb_it == local_carry.end()
                        || qa_it->second.last_seen_s != pl_now_s
                        || qb_it->second.last_seen_s != pl_now_s) {
                        skip(ctx.counters.n_lane_t4_skip_no_post, "t4_skip_no_post"); continue;
                    }
                    const double pre_lane  = ca.second->value_abs / m.lam1_m
                                           - cb.second->value_abs / m.lam2_m;
                    const double post_lane = qa_it->second.value_abs / m.lam1_m
                                           - qb_it->second.value_abs / m.lam2_m;
                    const double delta = post_lane - pre_lane - m.d_mw_cyc;
                    if (!std::isfinite(delta)) {
                        skip(ctx.counters.n_lane_t4_skip_bad_lambda, "t4_skip_bad_lambda"); continue;
                    }
                    ++ctx.counters.n_lane_t4_measured;
                    const double ad = std::abs(delta);
                    ctx.counters.lane_t4_abs_delta_sum_cyc += ad;
                    if (ad > ctx.counters.lane_t4_abs_delta_max_cyc)
                        ctx.counters.lane_t4_abs_delta_max_cyc = ad;
                    int bin = 8;
                    for (int bi = 0; bi < 8; ++bi) if (ad < kT4Bins[bi]) { bin = bi; break; }
                    ++ctx.counters.n_lane_t4_hist[bin];
                    row.measured = true;
                    row.delta_cyc = delta;
                    ctx.carry.relevel.lane_obs_rows.push_back(row);
                }
            }
            // One-epoch lifetime (invariant #1): a capture is consumed at the
            // FIRST harvest after its erase or dropped — never measured across
            // an unequal interval.
            ctx.carry.lane_t4_pre.clear();
        }
        // Stamp this harvest so the NEXT epoch can require a one-epoch-old pre.
        ctx.carry.pl_prev_now_s = pl_now_s;

        // Carry-retirement census: this prune is
        // the mechanism that produced the fleet-scale carry loss §1.3 called the
        // "cliff".  It reads ONLY last_seen_s — it selects on elapsed time and on
        // nothing else — so any claim that it performs quality THERAPY has to be
        // measured against the quality-selected retirement paths, not assumed.
        // The fleet size is captured BEFORE the loop so "fleet-scale" is a ratio.
        {
            const long long fleet_before = static_cast<long long>(local_carry.size());
            long long n_pruned = 0;
            for (auto it = local_carry.begin(); it != local_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) {
                    it = local_carry.erase(it);
                    ++n_pruned;
                } else {
                    ++it;
                }
            }
            if (n_pruned > 0) {
                ctx.counters.n_cr_gap_prune += n_pruned;
                ++ctx.counters.n_cr_gap_prune_epochs;
                if (n_pruned > ctx.counters.n_cr_gap_prune_max) {
                    ctx.counters.n_cr_gap_prune_max       = n_pruned;
                    // week/TOW, NOT pl_now_s: every trace CSV and log line in the
                    // run is keyed on TOW, so a continuous-GPS-seconds handle
                    // (~1.4e9) would match nothing the reader can grep.
                    ctx.counters.n_cr_gap_prune_max_week  =
                        static_cast<long long>(std::floor(pl_now_s / 604800.0));
                    ctx.counters.cr_gap_prune_max_tow     = std::fmod(pl_now_s, 604800.0);
                    ctx.counters.n_cr_gap_prune_max_fleet = fleet_before;
                }
            }
        }
        // Retap statistics-guard set: one RAIM-to-harvest lifetime, consumed by
        // the labeling above — cleared here so next epoch's RAIM refills it.
        ctx.carry.relevel_retapped_prev.clear();
        // --- I-in-block slant-iono carry (network.carry_iono_in_block): harvest each
        //     station's per-LOS iono posteriors THROUGH THE SAME per-column solve the
        //     ambiguities used, write their rows of the station's harvest block, stash the
        //     ABSOLUTE value, and prune the stale entries.
        //
        //     Independent loop (the ZWD pattern) so it runs for stations whose ambiguity
        //     harvest produced nothing.  Three deliberate differences from the QUEUE-8(b)
        //     diagonal carry's harvest, all of them the point of the card:
        //       * the CROSS terms are gathered (this is what subtracts the B-fold
        //         over-count at inject); QUEUE 8(b) took the diagonal alone,
        //       * the coupling row IS harvested — an in-block column is folded exactly
        //         like an ambiguity, border regression included, which is what makes the
        //         prior exact GIVEN the border (the rx-CB "no shared footprint" rationale
        //         is what the diagonal vehicle needed; it is not what this one needs),
        //       * ABSOLUTE value (x_lin + x_r).  network_ionosphere.csv writes x_r alone
        //         and is frame-broken; a carry that is not frame-complete would re-inject
        //         a correction against a moved linearization point.
        //     A LOS whose harvest fails keeps blk = nullptr, so every pair with it is
        //     zeroed and counted (n_pl_blk_iono_missing_cross) — the safe degrade.
        if (iono_blk_on) {
            auto& iono_blk_carry = ctx.carry.iono_blk_carry;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_iono.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> iblk = iblk_of_sta[i];
                const std::vector<Eigen::Index>* icross_cols =
                    iblk ? &iblk_cols_of_sta[i] : nullptr;
                const Eigen::Index irow0 = iblk ? iblk_row0_of_sta[i] : Eigen::Index{-1};
                std::vector<Eigen::Index> icols;
                icols.reserve(R.pl_iono.size());
                for (const auto& t : R.pl_iono) icols.push_back(std::get<1>(t));
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(est_idx, icols,
                                                                plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_iono.size(); ++a) {
                    const auto& [isat, icol, ix_lin] = R.pl_iono[a];
                    if (icol < 0 || icol >= xr.size()) continue;
                    const double value_abs = ix_lin + xr(icol);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    Eigen::VectorXd icross;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, icol, icross_cols,
                                                  iblk ? &icross : nullptr);
                        var = hc.first; coupling = std::move(hc.second);
                    } else {
                        var = (a < static_cast<std::size_t>(vard.size())) ? vard(a) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    const auto key = std::make_pair(static_cast<int>(i), isat);
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(coupling)};
                    // birth_ep / birth_s / var_first survive a re-stash and restart only
                    // on an erase-and-rebirth (a T4 erase, or a gap prune) — the ambiguity
                    // carry's rule, through the ambiguity carry's own helper, so the age
                    // of an iono carry means the same thing.  (This map is NOT
                    // snapshotted, so it never sees a RESTORED sentinel today; the shared
                    // helper is what makes that stay true if it ever is.)
                    const auto oit = iono_blk_carry.find(key);
                    const LocalCarry* iprev =
                        (oit != iono_blk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = iprev ? iprev->var_first : var;
                    localCarryStampBirth(
                        ent, iprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row irow0 + a of the station's harvest block = this iono column's
                    // n_c read at every carried column of the station (its own entry
                    // equals `var`, the pure conditional).  The entry then points at the
                    // SAME PlHarvestBlock the ambiguity and ZWD entries point at, so next
                    // epoch's inject pairs iono↔ambiguity, iono↔ZWD and iono↔iono by
                    // pointer equality exactly as it pairs ambiguity↔ambiguity.
                    if (iblk && irow0 >= 0 && icross.size() == iblk->C.rows()) {
                        const Eigen::Index irow = irow0 + static_cast<Eigen::Index>(a);
                        if (irow < iblk->C.rows()) {
                            iblk->C.row(irow) = icross.transpose();
                            ent.blk     = iblk;
                            ent.blk_idx = static_cast<int>(irow);
                        }
                    }
                    iono_blk_carry[key] = std::move(ent);
                }
            }
            // Invariant #1/#7 decoupling edge: a LOS unseen past the horizon is DROPPED,
            // never aged into a fold.  Counted — this is the eclipse / set edge, and a run
            // whose count is 0 over a full day did not exercise it.
            for (auto it = iono_blk_carry.begin(); it != iono_blk_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > iono_blk_gap_s) {
                    it = iono_blk_carry.erase(it);
                    ++ctx.counters.n_iono_blk_gap_drop;
                } else ++it;
            }
        }
        // --- RX-CLK-in-block receiver-clock carry (network.carry_rx_clock_in_block):
        //     harvest each station's receiver-clock (and ISB) posteriors THROUGH THE SAME
        //     per-column solve the ambiguities used, write their rows of the station's
        //     harvest block, run the (bias, drift) filter, and prune the stale entries.
        //
        //     ⚠ THE DRIFT IS THE WHOLE DESIGN.  The producer never writes its solution
        //     back into the station StateManager, so x_lin(clock) is set ONCE (0, or the
        //     latched cold_clock_seed median) and is CONSTANT for the run: x_r carries the
        //     entire clock, including a ~0.3 m/s ramp = 9 m per 30 s epoch (measured
        //     0.29994 m/s on the sim truth bed, with 0.299 m RW increments about it).  A
        //     bias-only random walk at q = 0.09 m² would therefore be ~30 σ wrong EVERY
        //     epoch.  The honest vehicle is the (bias, drift) pair: the block column holds
        //     the bias, and the drift is a per-station scalar Kalman state updated here
        //     from the harvested increments — the same state that supplies the one-step
        //     PREDICTION whose innovation is this card's clock-JUMP detector.
        //
        //     Independent loop (the ZWD/iono pattern) so it runs for stations whose
        //     ambiguity harvest produced nothing.  ISB columns share the loop but NOT the
        //     drift: an inter-system bias is a near-constant offset (the sim injects a
        //     fixed 8.000 m), so it takes a pure random walk.
        if (rxclk_blk_on) {
            auto& rx_clk_carry = ctx.carry.rx_clk_carry;
            auto& rx_clk_rate  = ctx.carry.rx_clk_rate;
            // The run's OWN observed harvest-to-harvest spacing, kept here rather than read
            // from ctx.carry.pl_prev_now_s because that field has ALREADY been advanced to
            // pl_now_s by the time this loop runs.  Used only to convert a gap into EPOCHS
            // for the per-epoch q, so the knob means the same thing at 1 s and at 30 s.
            double dt_ep_run = (ctx.carry.rxclk_prev_now_s > 0.0)
                                   ? pl_now_s - ctx.carry.rxclk_prev_now_s : 0.0;
            if (!std::isfinite(dt_ep_run) || dt_ep_run <= 0.0 || dt_ep_run > rxclk_gap_s)
                dt_ep_run = 0.0;   // first epoch / implausible spacing ⇒ gap_ep = 1
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_rxclk.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> rblk = rblk_of_sta[i];
                const std::vector<Eigen::Index>* rcross_cols =
                    rblk ? &rblk_cols_of_sta[i] : nullptr;
                const Eigen::Index rrow0 = rblk ? rblk_row0_of_sta[i] : Eigen::Index{-1};
                std::vector<Eigen::Index> rcols;
                rcols.reserve(R.pl_rxclk.size());
                for (const auto& t : R.pl_rxclk) rcols.push_back(t.col);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // Card 1 uses the PURE conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(est_idx, rcols,
                                                                plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_rxclk.size(); ++a) {
                    const PlRxClk& pr = R.pl_rxclk[a];
                    if (pr.col < 0 || pr.col >= xr.size()) continue;
                    const double value_abs = pr.x_lin + xr(pr.col);
                    if (!std::isfinite(value_abs)) continue;
                    double var;
                    std::vector<CoupledShared> coupling;
                    Eigen::VectorXd rcross;
                    if (carry_cpl_cond) {
                        auto hc = harvestCoupling(est_idx, pr.col, rcross_cols,
                                                  rblk ? &rcross : nullptr);
                        var = hc.first; coupling = std::move(hc.second);
                    } else {
                        var = (a < static_cast<std::size_t>(vard.size())) ? vard(a) : -1.0;
                    }
                    if (!(var > 0.0) || !std::isfinite(var)) continue;
                    const auto key = std::make_pair(static_cast<int>(i), pr.kind);
                    // --- (bias, drift) filter + the clock-JUMP response.  PRIMARY CLOCK
                    //     ONLY (pr.kind empty); an ISB has no rate by construction.
                    if (pr.kind.empty()) {
                        auto& rt = rx_clk_rate[key];   // default-constructs on first sighting
                        if (rt.prev_s > 0.0) {
                            const double dt = pl_now_s - rt.prev_s;
                            if (dt > 0.0 && dt <= rxclk_gap_s) {
                                const double gap_ep = (dt_ep_run > 0.0)
                                                          ? std::max(1.0, dt / dt_ep_run) : 1.0;
                                // rate_var < 0 = "never updated": the drift is UNKNOWN, so
                                // the prediction is bias-only with an uninformative rate.
                                // That is also what makes the jump gate correct on the very
                                // first increment — with σ_d = 1 m/s the 30 s prediction
                                // band is ~30 m, which a 9 m/epoch drift clears at 0.3 σ and
                                // a 1 ms receiver step (3e5 m) fails by four orders.
                                const double rvar_eff =
                                    (rt.rate_var >= 0.0) ? rt.rate_var : rxclk_rate_var0;
                                // The SAME kernel PASS B injected with, so the innovation
                                // is tested against the prediction that was actually used.
                                const RxClkPred rp = rxClockPredict(
                                    rt.prev_abs, rt.prev_var, rt.rate, rvar_eff, dt,
                                    gap_ep, rxclk_q);
                                const double var_pred = rp.var;
                                const double innov    = value_abs - rp.mean;
                                // Gate-scale census (invariant #4): the gate is
                                // k*sqrt(var_pred) METRES and var_pred is per
                                // station-epoch, so a report can quote the size an
                                // event must exceed rather than just k.  Measure-only.
                                if (var_pred > 0.0 && std::isfinite(innov)) {
                                    auto& C = ctx.counters;
                                    const double gate_m = rxclk_jump_k * std::sqrt(var_pred);
                                    if (C.n_rxclk_gate_tests == 0) {
                                        C.min_rxclk_gate_m = gate_m;
                                        C.max_rxclk_gate_m = gate_m;
                                    } else {
                                        C.min_rxclk_gate_m = std::min(C.min_rxclk_gate_m, gate_m);
                                        C.max_rxclk_gate_m = std::max(C.max_rxclk_gate_m, gate_m);
                                    }
                                    ++C.n_rxclk_gate_tests;
                                    C.sum_rxclk_gate_m += gate_m;
                                    C.max_rxclk_innov_m =
                                        std::max(C.max_rxclk_innov_m, std::abs(innov));
                                }
                                if (var_pred > 0.0 && std::isfinite(innov)
                                    && std::abs(innov) > rxclk_jump_k * std::sqrt(var_pred)) {
                                    // Invariant #1/#7: the receiver (or the solve) STEPPED.
                                    // DECOUPLING edge — erase BOTH halves so nothing crosses
                                    // the jump; RE-COUPLING edge — the next sighting is a
                                    // cold rebirth whose rate prior is uninformative again,
                                    // so the two epochs after a jump inject a near-free
                                    // prior rather than a stale one.
                                    rx_clk_carry.erase(key);
                                    rx_clk_rate.erase(key);
                                    ++ctx.counters.n_rxclk_carry_refused;
                                    continue;
                                }
                                // Scalar Kalman update of the drift from this increment.
                                // R_rate is the increment's own noise: two harvested
                                // variances plus the bias process noise, over Δt².
                                const double obs_rate = (value_abs - rt.prev_abs) / dt;
                                const double r_rate =
                                    (var + rt.prev_var + rxclk_q * gap_ep) / (dt * dt);
                                double rate_var_in = rvar_eff;
                                if (rxClockRateUpdate(rt.rate, rate_var_in, obs_rate,
                                                      r_rate, rxclk_rate_q * gap_ep)) {
                                    rt.rate_var = rate_var_in;
                                    ++rt.n_incr;
                                }
                            } else if (dt > rxclk_gap_s) {
                                // Past the horizon the drift extrapolation is not trusted
                                // either: reset the rate filter to its uninformative prior.
                                // The carry entry itself is erased by the prune below.
                                rt = {};
                            }
                        }
                        rt.prev_abs = value_abs;
                        rt.prev_s   = pl_now_s;
                        rt.prev_var = var;
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(coupling)};
                    // birth_ep / birth_s / var_first survive a re-stash and restart only on
                    // an erase-and-rebirth (a jump response, or a gap prune) — the ambiguity
                    // carry's rule, through the ambiguity carry's own helper, so the age of
                    // a clock carry means the same thing.  (This map is NOT snapshotted
                    // either; same note as the iono block above.)
                    const auto oit = rx_clk_carry.find(key);
                    const LocalCarry* rprev =
                        (oit != rx_clk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = rprev ? rprev->var_first : var;
                    localCarryStampBirth(
                        ent, rprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row rrow0 + a of the station's harvest block = this clock-family
                    // column's n_c read at every carried column of the station (its own
                    // entry equals `var`, the pure conditional).  The entry then points at
                    // the SAME PlHarvestBlock the ambiguity / iono / ZWD entries point at,
                    // so next epoch's inject pairs clock↔ambiguity, clock↔iono, clock↔ZWD
                    // by pointer equality exactly as it pairs ambiguity↔ambiguity.
                    if (rblk && rrow0 >= 0 && rcross.size() == rblk->C.rows()) {
                        const Eigen::Index rrow = rrow0 + static_cast<Eigen::Index>(a);
                        if (rrow < rblk->C.rows()) {
                            rblk->C.row(rrow) = rcross.transpose();
                            ent.blk     = rblk;
                            ent.blk_idx = static_cast<int>(rrow);
                        }
                    }
                    rx_clk_carry[key] = std::move(ent);
                }
            }
            // Invariant #1/#7 decoupling edge: a station unseen past the horizon is
            // DROPPED, never aged into a fold — and its drift state goes with it, because
            // extrapolating a rate across an unobserved hour is exactly the stale-prior
            // failure this card is built to avoid.  Counted.
            for (auto it = rx_clk_carry.begin(); it != rx_clk_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > rxclk_gap_s) {
                    rx_clk_rate.erase(it->first);
                    it = rx_clk_carry.erase(it);
                    ++ctx.counters.n_rxclk_carry_gap_drop;
                } else ++it;
            }
            ctx.carry.rxclk_prev_now_s = pl_now_s;
        }
        // --- ZWD carry (B9): harvest posterior ZWD + stash (mirrors the ambiguity
        //     harvest; independent loop so it works regardless of ambiguity presence).
        //     Runs for B9 proper, for a live post-resume seam window (rolling B9),
        //     AND — capture-only, feeding nothing back — on any snapshot-armed run,
        //     so every RTB-7 snapshot serializes fresh Z rows (§17 finding 3).  The
        //     capture path is read-only w.r.t. the solve: R.pl_zwd_present is
        //     record-only on those epochs and no inject ever fires.
        if (zwd_on || zwd_capture || zwd_seam_live) {
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
                // B9-in-block (network.carry_zwd_in_block): when this station's pl harvest
                // above put the ZWD column in `cols` (and only then), the ZWD is harvested
                // through the SAME cross-gather every ambiguity column used — one call,
                // one solve — and its row is written at block index n = cols.size()−1.
                // zblk is null on every other station-epoch and OFF ⇒ nullptr/nullptr,
                // i.e. the exact pre-card call (byte-identical).
                const std::shared_ptr<PlHarvestBlock> zblk =
                    zwd_blk_on ? zblk_of_sta[i] : nullptr;
                const std::vector<Eigen::Index>* zcross_cols =
                    zblk ? &zblk_cols_of_sta[i] : nullptr;
                Eigen::VectorXd zcross;
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, R.pl_zwd.col, zcross_cols,
                                              zblk ? &zcross : nullptr);
                    var = hc.first; coupling = std::move(hc.second);
                } else {
                    var = (vard.size() > 0) ? vard(0) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                // PRIOR-RESIDUAL CENSUS INC-2 (new hook): the ZWD carry has NO
                // screen of any kind today — no slip detector, no jump test, no
                // D5 analogue — which is precisely the standing-invariant-#1 gap
                // this card was built to inventory.  Unlike pl, `folded` is
                // GROUND TRUTH here and needs no reproduction: PASS B already
                // records R.pl_zwd_injected at the injector itself.  Read the
                // prior BEFORE the stash below overwrites it.  Additionally gated
                // on an inject-capable epoch (B9 or a live seam window) so the
                // capture-only path adds no rows — a snapshot-armed run's census
                // stays byte-identical to a pre-capture build's.
                if (prires_zwd && prires_est.enabled() && (zwd_on || zwd_seam_live)) {
                    const auto zit = zwd_carry.find(static_cast<int>(i));
                    if (zit != zwd_carry.end()) {
                        const double zgap = pl_now_s - zit->second.last_seen_s;
                        if (zgap >= 0.0 && zgap <= pl_max_gap_s) {
                            NetworkPriorResidual::Input in;
                            in.family   = "zwd";
                            in.station  = static_cast<int>(i);
                            in.e        = zit->second.value_abs - value_abs;
                            in.fold_var = zit->second.var + zwd_q * std::max(0.0, zgap);
                            in.post_var = var;
                            in.gap_s    = zgap;
                            in.folded   = R.pl_zwd_injected;
                            // A ZWD carry within its horizon that did NOT inject
                            // is not a mechanism RESPONSE — nothing fired on it;
                            // the injector simply had no live column or a
                            // non-positive var_eff.  CLEAN with the `+` branch is
                            // the honest classification, and the mech tag says so.
                            in.label = PriorResidualLabel::Clean;
                            in.mech  = R.pl_zwd_injected ? "" : "not_injected";
                            prires_est.addRow(in);
                        }
                    }
                }
                // §18.12 OBSERVATORY, ZWD half.  The Z rows are restored and counted
                // exactly like the L rows (n_resume_cpl_zrows, and they arm the same
                // resume_cpl_census_pending latch), so a census that covered only the L
                // side would report rel ≈ 0 on a leg whose ZWD regression was stale --
                // the invariant-#4 shape where the instrument reads clean because it never
                // looked.  Same measure-only body as the phase-leveling census above; read
                // BEFORE the stash below moves `coupling` out.
                if (ctx.carry.resume_cpl_census_pending && carry_cpl_cond
                    && !coupling.empty()) {
                    const auto zold = zwd_carry.find(static_cast<int>(i));
                    if (zold != zwd_carry.end() && !zold->second.coupling.empty()) {
                        const auto idOfZ = [](const CoupledShared& c) {
                            return std::make_tuple(static_cast<int>(c.kind), c.sat,
                                                   c.signal, c.n, c.m,
                                                   static_cast<int>(c.trig), c.sh_layer);
                        };
                        std::map<decltype(idOfZ(coupling.front())), double> znow;
                        double zmaxabs = 0.0, zrhs_new = 0.0, zrhs_old = 0.0, zdmax = 0.0;
                        for (const CoupledShared& cs : coupling) {
                            znow[idOfZ(cs)] = cs.coeff;
                            zmaxabs  = std::max(zmaxabs, std::abs(cs.coeff));
                            zrhs_new += cs.coeff * cs.s_prev;
                        }
                        long long zmatched = 0, zgone = 0;
                        for (const CoupledShared& cs : zold->second.coupling) {
                            zrhs_old += cs.coeff * cs.s_prev;
                            const auto h = znow.find(idOfZ(cs));
                            if (h == znow.end()) { ++zgone; continue; }
                            ++zmatched;
                            zdmax = std::max(zdmax, std::abs(cs.coeff - h->second));
                        }
                        ++ctx.counters.n_resume_cpl_census_rows;
                        ctx.counters.n_resume_cpl_census_terms     += zmatched;
                        ctx.counters.n_resume_cpl_census_unmatched += zgone;
                        if (zmaxabs > 0.0)
                            ctx.carry.resume_cpl_census_rel.push_back(zdmax / zmaxabs);
                        ctx.carry.resume_cpl_census_rhs.push_back(
                            std::abs(zrhs_new - zrhs_old));
                    }
                }
                zwd_carry[static_cast<int>(i)] =
                    LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
                // B9-in-block: row n of the station's harvest block = the ZWD column's own
                // n_c read at every carried column (its last entry equals `var`, the pure
                // conditional).  The entry then points at the SAME PlHarvestBlock the
                // ambiguity entries point at, so next epoch's inject pairs ZWD↔ambiguity by
                // pointer equality exactly as it pairs ambiguity↔ambiguity — and with
                // nothing stashed at a different harvest.  A ZWD whose harvest failed above
                // never reaches here: its entry keeps blk = nullptr and every pair with it
                // is zeroed + counted (pl_blk_zwd_missing_cross), which is the safe degrade.
                if (zblk && zcross.size() == zblk->C.rows()) {
                    const Eigen::Index zrow = zblk->C.rows() - 1;   // the appended column
                    zblk->C.row(zrow) = zcross.transpose();
                    auto& zent = zwd_carry[static_cast<int>(i)];
                    zent.blk     = zblk;
                    zent.blk_idx = static_cast<int>(zrow);
                }
            }
            for (auto it = zwd_carry.begin(); it != zwd_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) {
                    it = zwd_carry.erase(it);
                    ++ctx.counters.n_cr_zwd_gap_prune;   // same predicate, same census
                }
                else ++it;
            }
            // Seam-window bookkeeping (network.zwd_seam_reseed): count this epoch's
            // actual folds (R.pl_zwd_injected is the injector's own ground truth),
            // advance the retirement clock, and log the DECOUPLING edge when the
            // horizon exhausts (invariant #7 — the ARMED line at the resume was the
            // re-coupling edge).  Serial section: the parallel produce phase only
            // ever READS zwd_seam_ep_left.
            if (zwd_seam_live) {
                long long seam_folds = 0;
                for (size_t i = 0; i < stations.size(); ++i)
                    if (est_index_of[i] >= 0 && asm_res[i].pl_zwd_injected) ++seam_folds;
                ctx.counters.n_zwd_seam_folds += seam_folds;
                if (seam_folds > 0) ++ctx.counters.n_zwd_seam_epochs;
                if (--ctx.carry.zwd_seam_ep_left == 0) {
                    // This seam has no logger parameter — resolve per call and
                    // null-guard (the spdlog free-function / cached-static hazard:
                    // never cache, never deref unguarded).
                    if (auto lg = spdlog::get("main_log"))
                        lg->info("Network (ZWD seam re-seed): RETIRED — the {}-epoch "
                                 "post-resume window is exhausted at tow {:.1f}; {} "
                                 "station-fold(s) over {} epoch(s) rode the window.  The "
                                 "station filters carry their own ZWD memory from here; "
                                 "continuous epochs never carry.",
                                 ctx.knobs.zwd_seam_epochs,
                                 std::fmod(pl_now_s, 604800.0),
                                 ctx.counters.n_zwd_seam_folds,
                                 ctx.counters.n_zwd_seam_epochs);
                }
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
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) {
                    it = rpb_carry.erase(it);
                    ++ctx.counters.n_cr_rpb_gap_prune;   // same predicate, same census
                } else ++it;
            }
        }
        // --- Slant-iono carry (QUEUE 8b): harvest posterior per-LOS iono + stash
        //     + prune.  Mirrors the RPB harvest (plural-column, diagonal
        //     conditional variance — DELIBERATELY no Card-1 coupling harvest, the
        //     rx-CB rationale).  Stashes EVERY epoch regardless of the inject
        //     scope, so the carried value tracks the solution continuously and
        //     the re-draw-event inject always has a fresh pre-event value.
        if (iono_on) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_iono.empty()) continue;
                const std::size_t est_idx =
                    static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                std::vector<Eigen::Index> cols;
                cols.reserve(R.pl_iono.size());
                for (const auto& t : R.pl_iono) cols.push_back(std::get<1>(t));
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        est_idx, cols, plan.shared_datum);
                } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_iono.size(); ++a) {
                    const auto& [isat, icol, ix_lin] = R.pl_iono[a];
                    if (icol < 0 || icol >= xr.size()) continue;
                    double value_abs, var;
                    if (ctx.knobs.iono_source_smoothed) {
                        // CS-GF authority (§6.2 successor): stash the
                        // level-independent carrier-smoothed GF-code estimate
                        // instead of the solve's (level-locked) iono.  A LOS
                        // whose authority is uninitialized this epoch keeps its
                        // old carry aging toward eviction — never a stale stash.
                        const auto csit = ctx.carry.iono_cs.find(
                            {static_cast<int>(i), isat});
                        if (csit == ctx.carry.iono_cs.end()
                            || csit->second.n <= 0) continue;
                        value_abs = csit->second.val;
                        var = std::max(csit->second.var, 1e-6);
                    } else {
                        value_abs = ix_lin + xr(icol);
                        var = (a < static_cast<std::size_t>(vard.size()))
                                  ? vard(a) : -1.0;
                    }
                    if (!std::isfinite(value_abs) || !(var > 0.0)
                        || !std::isfinite(var)) continue;
                    // PRIOR-RESIDUAL CENSUS, family "iono": the ONE instrument
                    // that can see a wrong carried iono value (T4 pools phase
                    // rows only; the prefit census is x_lin-framed; phase is
                    // self-consistent with a level re-leveled against the wrong
                    // carry).  `folded` is ground truth from the injector
                    // (iono_injected_sats).  Read the prior BEFORE the stash
                    // below overwrites it.
                    if (prires_iono && prires_est.enabled()) {
                        const auto pit = iono_carry.find(
                            {static_cast<int>(i), isat});
                        if (pit != iono_carry.end()) {
                            const double igap =
                                pl_now_s - pit->second.last_seen_s;
                            if (igap >= 0.0 && igap <= iono_max_gap_s) {
                                NetworkPriorResidual::Input in;
                                in.family   = "iono";
                                in.sat      = isat;
                                in.station  = static_cast<int>(i);
                                in.e        = pit->second.value_abs - value_abs;
                                in.fold_var = std::max(
                                    pit->second.var + iono_q * std::max(0.0, igap),
                                    iono_var_floor);
                                in.post_var = var;
                                in.gap_s    = igap;
                                in.folded   = R.iono_injected_sats.count(isat) > 0;
                                in.label = PriorResidualLabel::Clean;
                                in.mech  = in.folded ? "" : "not_injected";
                                prires_est.addRow(in);
                            }
                        }
                    }
                    iono_carry[{static_cast<int>(i), isat}] =
                        LocalCarry{value_abs, var, pl_now_s};
                }
            }
            for (auto it = iono_carry.begin(); it != iono_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > iono_max_gap_s) {
                    it = iono_carry.erase(it);
                    ++ctx.counters.n_iono_carry_evicted;
                } else ++it;
            }
        }
    }

    // --- RXCB-DATUM gauge census ------------------
    // The zero-mean receiver-DCB datum's fold count, in its OWN walk, gated on its
    // OWN knob.  network.rx_code_bias_datum and network.carry_rx_code_bias are
    // independent, and datum-WITHOUT-carry is the canonical posture; counting this
    // inside the carry bracket below left n_rxcb_gauges at 0 on every such run, so
    // the run-end RXCB-DATUM line fired its standing-invariant-#4 alarm against a
    // HEALTHY gauge.  A false negative on the one instrument whose whole job is to
    // tell a live gauge from an armed-but-inert one is worse than no instrument.
    //
    // This is now the SOLE accumulation site, so an armed-both run cannot double
    // count.  It is also byte-identical on the carry-only posture: R.pl_cb_datum_rows
    // is incremented only under `rx_cb_datum` (ApplicationNetworkAssembly.cpp, the
    // RXCB-DATUM row build), so it is structurally 0 whenever this walk is skipped.
    if (ctx.knobs.rx_cb_datum) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            ctx.counters.n_rxcb_gauges +=
                static_cast<size_t>(asm_res[i].pl_cb_datum_rows);
        }
    }

    // --- rx-CB carry: harvest + stash + prune ------
    // DELIBERATELY OUTSIDE the carry_warm bracket above: the receiver code-bias
    // carry must be live on the carry_invariant/no-phase-leveling lineage (the
    // carry_warm gate is what makes the ZWD carry inert there).  Mirrors the RPB
    // harvest mechanics: plural columns per station, conditional local variance,
    // LocalCarry stash keyed (station, CB key).  The produce phase only READS the
    // map (find, never insert/erase) and counts its decisions in R — summed here
    // serially, so the whole cycle is race-free by construction.
    if (ctx.knobs.rx_cb_on) {
        auto& rx_cb_carry = ctx.carry.rx_cb_carry;
        constexpr double kRxCbMaxGapS = 900.0;   // spec §3: housekeeping horizon
        // Card LV-4: this epoch's invariant steps, collected during the station walk
        // and judged AFTER it — the screen needs the whole population to build a
        // robust, peer-relative scale (rxCbStepScale).  Empty unless armed.
        struct Lv4Pending { int station; std::string key; double step; double formal_sigma; };
        std::vector<Lv4Pending> lv4_pending;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            ctx.carry.rx_cb_injected_total += R.pl_cb_injected;
            // n_rxcb_gauges is NOT summed here — it belongs to the RXCB-DATUM knob,
            // not the carry, and lives in its own walk above.  Every counter that
            // remains in this bracket is genuinely carry-only: their producer-side
            // increments all sit BELOW the `if (!rx_cb_on) continue;` in the
            // assembly's pl_cb walk, so a datum-only run cannot populate them.
            ctx.carry.rx_cb_innov_skips_total += R.pl_cb_innov_skips;
            ctx.carry.lv2_folds_demeaned += R.pl_cb_demeaned;   // Card LV-2
            // Card LV-4 same-run A/B — summed with the other produce-phase tallies,
            // BEFORE any early continue, so a station whose back-substitution throws
            // still reports what the retired screen would have done to its folds.
            ctx.carry.lv4_frame_gate_would_skip += R.pl_cb_frame_gate_would_skip;
            if (R.pl_cb.empty()) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            Eigen::VectorXd xr, vard;
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_cb.size());
            for (const auto& t : R.pl_cb) cols.push_back(std::get<1>(t));
            try {
                xr = est.reconstructStation(est_idx, x_s);
                vard = est.conditionalLocalVarianceDiag(
                    est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < R.pl_cb.size(); ++a) {
                const auto& [cb_key, ccol, cx_lin] = R.pl_cb[a];
                if (ccol < 0 || ccol >= xr.size()) continue;
                const double value_abs = cx_lin + xr(ccol);
                const double var = (a < static_cast<std::size_t>(vard.size()))
                                       ? vard(a) : -1.0;
                if (!std::isfinite(value_abs) || !(var > 0.0)
                    || !std::isfinite(var)) continue;
                // PRIOR-RESIDUAL CENSUS INC-2 (new hook).  `folded` is RE-DERIVED
                // by calling rxCbInjectGate with the identical arguments PASS B
                // used — the gate is a pure function and every input is still in
                // hand and un-mutated at this point (the stash below is what
                // overwrites the entry), so this reproduces the injector's own
                // decision exactly rather than mirroring it.  The SkipInnov branch
                // is a genuine invariant-#1 event RESPONSE (receiver reset /
                // firmware bias jump: withhold the fold, let the harvest re-seed),
                // so it is the RESPONDED positive control for this family.
                if (prires_rxcb && prires_est.enabled()) {
                    const auto pit = rx_cb_carry.find({static_cast<int>(i), cb_key});
                    if (pit != rx_cb_carry.end()) {
                        const double cgap = pl_now_s - pit->second.last_seen_s;
                        const double cinnov = pit->second.value_abs - cx_lin;
                        double cvar_eff = 0.0;
                        const RxCbInjectAction cact = rxCbInjectGate(
                            cgap, kRxCbMaxGapS, pit->second.var, ctx.knobs.rx_cb_q,
                            cinnov, /*k_sigma=*/10.0, /*guard_var=*/0.25, cvar_eff,
                            // Mirror the injector's actual configuration, or
                            // the census would label rows "responded" for a test the
                            // run no longer applies.
                            /*innov_test=*/!ctx.knobs.rx_cb_step_gate);
                        // SkipGap leaves cvar_eff at 0 (the gate returns before
                        // setting it) — there is no fold variance to standardize
                        // against, so fall back to the nominal recompute and mark
                        // the row RAW FORMAL rather than invent a scale.
                        const bool have_var = cvar_eff > 0.0;
                        NetworkPriorResidual::Input in;
                        in.family   = "rxcb";
                        in.signal   = cb_key;
                        in.station  = static_cast<int>(i);
                        in.e        = pit->second.value_abs - value_abs;
                        in.fold_var = have_var
                            ? cvar_eff
                            : pit->second.var
                                + ctx.knobs.rx_cb_q * std::max(0.0, cgap);
                        in.post_var = var;
                        in.gap_s    = std::max(0.0, cgap);
                        in.folded   = (cact == RxCbInjectAction::Fold);
                        in.var_nominal = !have_var;
                        in.label = (cact == RxCbInjectAction::SkipInnov)
                                       ? PriorResidualLabel::Responded
                                       : PriorResidualLabel::Clean;
                        in.mech  = (cact == RxCbInjectAction::SkipInnov) ? "innov_withhold"
                                 : (cact == RxCbInjectAction::SkipGap)   ? "gap" : "";
                        prires_est.addRow(in);
                    }
                }
                // --- Card LV-4: the FRAME-INVARIANT event screen -------------
                // docs/SPEC_level_information_budget.md.  This is the only place in
                // the cycle where TWO absolute realizations of the same receiver bias
                // exist side by side — the freshly back-substituted posterior
                // (cx_lin + xr) and the carried one — so their difference is the one
                // discrepancy no reframing, reseeding or datum change can move.  The
                // retired inject test compared the carried value against a FROZEN
                // x_lin instead, which is why it withheld half the folds of stations
                // that never did anything and stayed blind to the jumps it was built
                // for (at inject time the carried value is still the pre-jump one).
                //
                // The DECISION cannot be taken here: it needs this epoch's whole
                // population (see rxCbStepScale — a formal-σ test mass-triggers, and
                // a step common to every station is a frame move, not 61 receiver
                // resets).  So the step is recorded and the screen runs after this
                // station loop, over all stations at once.
                if (ctx.knobs.rx_cb_step_gate) {
                    const auto sit = rx_cb_carry.find({static_cast<int>(i), cb_key});
                    if (sit != rx_cb_carry.end()) {
                        lv4_pending.push_back(
                            {static_cast<int>(i), cb_key,
                             value_abs - sit->second.value_abs,
                             std::sqrt(std::max(0.0,
                                 var + sit->second.var + ctx.knobs.rx_cb_q
                                     * std::max(0.0, pl_now_s - sit->second.last_seen_s)))});
                    }
                }
                rx_cb_carry[{static_cast<int>(i), cb_key}] =
                    LocalCarry{value_abs, var, pl_now_s};
                ++ctx.carry.rx_cb_harvested_total;
            }
        }
        // --- Card LV-4, decision pass (serial, all stations) -------------------
        // Robust and PEER-RELATIVE, per code-bias family: σ̂ = 1.4826·MAD of this
        // epoch's steps (floored at the median formal σ), and the MEDIAN step is
        // removed first because a step common to the network is a frame move, not an
        // event.  What is left is the per-station outlier.  Response is ERASE, not
        // overwrite (reset-not-repair): a genuine jump means this epoch's solve
        // folded a stale pre-jump prior, so the posterior it produced is suspect
        // too — the entry goes cold for exactly one epoch and the next harvest
        // re-seeds it from data, the same lifecycle a first sighting takes.
        if (ctx.knobs.rx_cb_step_gate && !lv4_pending.empty()) {
            std::map<std::string, std::vector<double>> by_fam, formal_by_fam;
            for (const auto& p : lv4_pending) {
                by_fam[p.key].push_back(p.step);
                formal_by_fam[p.key].push_back(p.formal_sigma);
            }
            std::map<std::string, RxCbStepScale> scale;
            for (auto& [fam, steps] : by_fam) {
                auto& f = formal_by_fam[fam];
                std::nth_element(f.begin(), f.begin() + f.size() / 2, f.end());
                // min_n mirrors the LV-2 thin-mean floor: below it there is no
                // defensible "normal", so the screen stands down (counted).
                scale[fam] = rxCbStepScale(steps, f[f.size() / 2],
                                           ctx.knobs.rx_cb_demean_min_sta);
            }
            for (const auto& p : lv4_pending) {
                const auto sc = scale.find(p.key);
                if (sc == scale.end()) continue;
                ++ctx.carry.lv4_step_checks;
                if (!sc->second.valid) { ++ctx.carry.lv4_scale_standdown; continue; }
                const RxCbStepVerdict sv =
                    rxCbStepVerdict(p.step, sc->second, ctx.knobs.rx_cb_step_k);
                ctx.carry.lv4_max_step_z = std::max(ctx.carry.lv4_max_step_z, sv.z);
                ctx.carry.lv4_max_scale_m =
                    std::max(ctx.carry.lv4_max_scale_m, sc->second.sigma);
                if (!sv.jumped) continue;
                rx_cb_carry.erase({p.station, p.key});
                ++ctx.carry.lv4_step_resets;
            }
        }
        for (auto it = rx_cb_carry.begin(); it != rx_cb_carry.end();) {
            if (pl_now_s - it->second.last_seen_s > kRxCbMaxGapS)
                it = rx_cb_carry.erase(it);
            else ++it;
        }
        // --- Card S2H: the S-consistent harvest (serial, all stations) ---------
        // docs/SPEC_level_information_budget.md §17 item 3 / §27 item 1.  Runs
        // AFTER the LV-4 erase pass and the gap prune, so the member set is
        // exactly the set that can FOLD next epoch — the S-transform re-realizes
        // the carried representation that will actually exert force.  Per family
        // (the CB state key: constellation + DCB/IFB kept apart, the LV-2 rule),
        // subtract the Λ-weighted mean of the harvested ABSOLUTE realizations
        // from every entry, fresh and stale-but-alive alike: the whole carried
        // set moves to the zero-mean frame together, so next epoch's folds exert
        // ZERO net force on the common mode in the fold's own metric — the datum
        // row (whose target is the same absolute zero) owns the level, and the
        // frame is re-derived from THIS epoch's solution.  No frozen pin (LV-2's
        // failure: it pinned mean(x_lin)), no lag-1 feedback (nothing is fed
        // back), no cross-station prior rows (the carry stays diagonal — this is
        // LASER Net3DLsa::applySTransformationForBigSystemOfEquations, Λ-weighted;
        // see rxCbHarvestMu for the two documented deviations).
        //
        // Covariance is co-transformed exactly (diag of S·Q·Sᵀ for diagonal Q):
        // voters get var − 1/Σλ (their uncertainty ABOUT THE NEW FRAME is smaller
        // — the common-mode variance left with μ), screened non-voters get
        // var + 1/Σλ (μ's own variance adds, independence).  The floor guards the
        // w→1 degenerate corner where one member IS the frame; with min_n ≥ 2 and
        // comparable weights it never binds (1/Σλ ≈ var/n).
        if (ctx.knobs.rx_cb_harvest_demean && !rx_cb_carry.empty()) {
            std::map<std::string, std::vector<LocalCarry*>> fam_entries;
            for (auto& [key, lc] : rx_cb_carry)
                fam_entries[key.second].push_back(&lc);
            bool any_applied = false;
            ctx.carry.rx_cb_harvest_last.clear();
            for (auto& [fam, entries] : fam_entries) {
                std::vector<double> vals, vars;
                vals.reserve(entries.size());
                vars.reserve(entries.size());
                for (const LocalCarry* e : entries) {
                    vals.push_back(e->value_abs);
                    vars.push_back(e->var);
                }
                std::vector<unsigned char> used;
                const RxCbHarvestReduce red = rxCbHarvestMu(
                    vals, vars, ctx.knobs.rx_cb_demean_min_sta,
                    ctx.knobs.rx_cb_harvest_screen_z, &used);
                auto& rec = ctx.carry.rx_cb_harvest_last[fam];
                rec.mu = red.mu; rec.sum_lam = red.sum_lam; rec.n = red.n;
                rec.n_used = red.n_used; rec.n_screened = red.n_screened;
                rec.applied = red.applied;
                if (!red.applied) { ++ctx.carry.s2h_thin_skips; continue; }
                const double dvar = 1.0 / red.sum_lam;
                for (std::size_t e = 0; e < entries.size(); ++e) {
                    LocalCarry* lc = entries[e];
                    if (std::isfinite(lc->value_abs)) lc->value_abs -= red.mu;
                    if (std::isfinite(lc->var) && lc->var > 0.0) {
                        if (e < used.size() && used[e])
                            lc->var = std::max(lc->var - dvar, 0.1 * lc->var);
                        else
                            lc->var += dvar;
                    }
                }
                ctx.carry.s2h_screened_total += red.n_screened;
                ctx.carry.s2h_last_mu_m = red.mu;
                ctx.carry.s2h_max_abs_mu_m =
                    std::max(ctx.carry.s2h_max_abs_mu_m, std::abs(red.mu));
                ctx.carry.s2h_mu_abs_sum += std::abs(red.mu);
                ++ctx.carry.s2h_mu_n;
                any_applied = true;
            }
            if (any_applied) ++ctx.carry.s2h_epochs_applied;
        }
        // --- Card LV-2: the common-mode mean, computed HERE (serial) -----------
        // docs/SPEC_level_information_budget.md.  The parallel produce phase
        // accumulated, per station and per constellation, Σλ·innov and Σλ over the
        // rows it actually FOLDED (the gate's skips contribute nothing — a withheld
        // row exerts no force, so it must not vote on the force being removed).
        // Reducing them here and applying the result LAG-1 keeps the whole cycle
        // race-free by construction, exactly like the carry itself — but the lag is
        // a feedback delay, so under card LV-5 μ is instead reduced BEFORE
        // PASS B (networkRxCbDemeanPrepass) and this pass degrades to a diagnostic;
        // see the branch below.
        //
        // μ_c = Σλ·innov / Σλ  ⇒  Σλ·(innov − μ_c) = 0 in DELTA coordinates: the
        // carry keeps every between-station DIFFERENCE at full stiffness and stops
        // re-asserting LAST epoch's common-mode realization (the flywheel).
        //
        // ⚠ It does NOT free that direction (measured 2026-08-05, SPEC §13).  The
        // de-mean moves the RHS, never the fold's information, so Σλ ≈ 100 units
        // still land on the common mode — and in ABSOLUTE terms each row now targets
        // value_abs−μ, whose weighted mean is mean(x_lin).  So the de-meaned carry
        // PINS the receiver-DCB common mode to the frozen first-epoch code-bias seed
        // frame: a rolling memory swapped for a frozen one.  It is therefore closer
        // to the rejected "demeaned target" variant than this card believed — the
        // difference being that the pinned value is epoch 1's data-derived mean
        // rather than the physical zero, which is exactly why card LV-1 (which sets
        // that seed through initializeCodeBiases) moves the level ~10 TECU with no
        // row-set change at all.
        if (ctx.knobs.rx_cb_demean) {
            std::map<std::string, double> sum_lam_innov, sum_lam;
            std::map<std::string, int> n_sta;
            for (std::size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                for (const auto& [fam, s] : R.pl_cb_sum_lam) {
                    if (!(s > 0.0)) continue;
                    const auto nit = R.pl_cb_sum_lam_innov.find(fam);
                    if (nit == R.pl_cb_sum_lam_innov.end()) continue;
                    sum_lam[fam] += s;
                    sum_lam_innov[fam] += nit->second;
                    ++n_sta[fam];
                }
            }
            // Invariant #7 (both edges): a DECOUPLING epoch — mass re-seed, RAIM
            // re-reduce, a thin station set — must not be allowed to define the
            // frame.  Below the floor μ FREEZES at its previous value (a held frame
            // is conservative: it re-asserts nothing new) and the event is COUNTED,
            // so a run that froze its way through an era says so.
            // Card LV-5: with the within-epoch reduction armed, THIS pass is a
            // DIAGNOSTIC only.  μ was already formed between PASS A and PASS B and
            // folded against; what the folded set's raw mean measures now is the
            // RESIDUAL common-mode force that survived the correction — the direct
            // check that the de-mean did its job (it is not identically zero because
            // the folded set is not exactly the candidate set the prepass averaged).
            // Overwriting μ here would silently reinstate the lag.
            const bool lv5_within = ctx.knobs.rx_cb_demean_within;
            bool any = false;
            if (!lv5_within) ctx.carry.rx_cb_demean_last.clear();
            for (const auto& [fam, s] : sum_lam) {
                const auto held = ctx.carry.rx_cb_demean_mu.find(fam);
                const double prev_mu =
                    (held != ctx.carry.rx_cb_demean_mu.end()) ? held->second : 0.0;
                const RxCbDemeanReduce red = rxCbDemeanMu(
                    sum_lam_innov[fam], s, n_sta[fam],
                    ctx.knobs.rx_cb_demean_min_sta, prev_mu);
                if (lv5_within) {
                    if (!red.fresh) continue;
                    // Residual = the folded set's raw common mode minus the μ that was
                    // actually applied to it.
                    const double applied = prev_mu;
                    const double resid = red.mu - applied;
                    ctx.carry.lv5_resid_sum += std::abs(resid);
                    ctx.carry.lv5_max_abs_resid_m =
                        std::max(ctx.carry.lv5_max_abs_resid_m, std::abs(resid));
                    ++ctx.carry.lv5_resid_n;
                    auto it_rec = ctx.carry.rx_cb_demean_last.find(fam);
                    if (it_rec != ctx.carry.rx_cb_demean_last.end())
                        it_rec->second.folded_mu = red.mu;
                    continue;
                }
                auto& rec = ctx.carry.rx_cb_demean_last[fam];
                rec.sum_lam = s;
                rec.n_sta   = n_sta[fam];
                rec.fresh   = red.fresh;
                rec.mu      = red.mu;
                if (!red.fresh) { ++ctx.carry.lv2_mu_frozen; continue; }
                ctx.carry.rx_cb_demean_mu[fam] = red.mu;
                ctx.carry.lv2_last_mu_m = red.mu;
                ctx.carry.lv2_max_abs_mu_m =
                    std::max(ctx.carry.lv2_max_abs_mu_m, std::abs(red.mu));
                any = true;
            }
            if (any) ++ctx.carry.lv2_mu_epochs;
        }
    }

    // --- Card LV-1: LAG-1 snapshot of the solved SH-VTEC field ----------------
    // docs/SPEC_level_information_budget.md.  Copied SERIALLY here (post-solve,
    // x_s final) and read next epoch by the PARALLEL produce phase, which samples
    // it at each new LOS's pierce point so a per-LOS iono state is born in the
    // FIELD's frame instead of the receiver-bias-in frame the raw geometry-free
    // code seed carries.  Coefficients only: the pierce geometry and the sun-fixed
    // rotation belong to the epoch that EVALUATES them, not to the epoch that
    // solved them.  Cheap ((N+1)² doubles/shell, 81 at degree 8) and untouched
    // when the card is disarmed.
    if (ctx.knobs.lv1_seed_frame && sh_model) {
        const std::vector<ShLayer> lv1_lay =
            shLayers(config_.getGlobalVtecShConfig(), sh_model, sh_model2);
        std::vector<std::vector<double>> snap(lv1_lay.size());
        bool ok = !lv1_lay.empty();
        for (std::size_t l = 0; l < lv1_lay.size() && ok; ++l) {
            const auto& cf = lv1_lay[l].model->coeffs();
            snap[l].resize(cf.size(), 0.0);
            for (std::size_t k = 0; k < cf.size() && ok; ++k) {
                try {
                    const Eigen::Index col = est.vtecCoeffColumn(
                        cf[k].n, cf[k].m, cf[k].trig, lv1_lay[l].layer);
                    if (col < 0 || col >= x_s.size()) { ok = false; break; }
                    const double v = x_s(col);
                    if (!std::isfinite(v)) { ok = false; break; }
                    snap[l][k] = v;
                } catch (...) { ok = false; }
            }
        }
        // A partial / failed read leaves the PREVIOUS snapshot standing (or none at
        // all on epoch 1, which falls back to the a₀₀ climatology prior mean —
        // the designed level authority).  Never install a half-built field.
        if (ok) {
            ctx.carry.sh_field_lag1 = std::move(snap);
            ++ctx.carry.lv1_field_snapshots;
        }
    }

    // §18.12: retire the first-warm-epoch latch at the END of this epoch's SERIAL
    // harvest.  Everything the latch gates — the C1 diagonal-border count and the
    // present/departed fold split (both earlier in the epoch, in the assembly) and the
    // restored-vs-fresh regressor census above — therefore covers EXACTLY the first
    // post-resume epoch.  Unconditional: a run that never resumed leaves it false, and
    // clearing a false bool is a no-op on every OFF path.
    if (ctx.carry.resume_cpl_census_pending)
        // RS-7 (§21.14), P9a's second instrument.  The warm resume runs BEFORE any epoch,
        // so every Q3 decouple counted by now belongs to the first post-resume epoch —
        // no baseline subtraction, and no per-epoch plumbing.  §18.11 measured 33 of 52
        // anchored clock priors out-voted at the first fold when the seam's carried rows
        // met a border with no cross-covariance; restoring the border is supposed to put
        // this back at the continuous run's level.
        ctx.counters.n_resume_first_fold_cta_trips =
            ctx.counters.cta_trip_nis + ctx.counters.cta_trip_abs
          + ctx.counters.cta_trip_gap + ctx.counters.cta_trip_datum
          + ctx.counters.cta_trip_ext;
    ctx.carry.resume_cpl_census_pending = false;
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
                       NetworkRunTraces& traces, const GnssTime& t,
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
    // READMIT (E2): keep the outgoing stash so departures can be PARKED after the rebuild.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    std::vector<CarryPrior> old_carry;
    if (readmit_h > 0.0) old_carry = prev_carry;
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
    // Carry this stash epoch's clock-monitor flags across to the next
    // inject (hard-refloat mode skip-stashes the flagged sat entirely, so the
    // flag cannot ride on prev_carry.inflate_q).  Gated to keep OFF runs pristine.
    // Card 1g honours the same history with κ off, so the mirror must follow the wider
    // predicate — otherwise arming 1g alone would leave the clock-QC half of the feed
    // permanently empty and the widening would silently cover only the reref sources.
    if (carry_cpl_tu || ctx.knobs.carry_cpl_reinit)
        prev_clock_refloat_sats = clock_refloat_sats;
    // The satellite clock is stashed to prev_carry ONLY in scalar-carry mode.  Under
    // network.clock_model the clock is carried by clock_osc (updated above), so it is not
    // stashed here — the two-state model is its sole cross-epoch memory.
    if (!clk_model_on)
        for (const std::string& sat : registered_clock_sats) {
            const bool flagged = clock_refloat_sats.count(sat) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
        }
    // eq34 reattach hygiene INC-2: refresh the
    // bridge's value source from THIS kept epoch's solved shared phase biases.
    // Rebuilt wholesale in lock-step with the D2 carry (a departed key drops out, so a
    // bridge can never be opened on a level from before an absence), and harvested
    // HERE — at the stash, which every whole-epoch withhold path `continue`s before
    // reaching — so a withheld epoch's transient solve never becomes a held level.
    // Deliberately NOT read off prev_carry: under carry_invariant the shared PhaseBias
    // carry is not stashed at all, which is exactly the stack this card was
    // root-caused on.  The SBC completion (network.sbasis_complete) re-uses this
    // harvest for the same reason — its continuity targets need the last SOLVED
    // phase-bias levels on exactly the carry_invariant stacks.  Empty / untouched
    // when neither consumer is armed.
    const bool harvest_pb_levels =
        ctx.knobs.pin_bridge_on || ctx.knobs.sbasis_complete_on;
    if (harvest_pb_levels) ctx.carry.pb_level_prev.clear();
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // §9.7.10 datum-station slip re-reference (network.cycle_slip.datum_station_detect):
        // a slip at the gauge-pinned datum receiver moved the level every phase bias
        // of this satellite references.  HARD-skip the stash for one kept epoch —
        // BEFORE the pb_level_prev harvest (a bridge must not re-open on the stale
        // level) and before both stash branches (plain and phase_hold) — so the next
        // solve re-seeds the bias at the new pivot level, in lockstep with the
        // network-wide ambiguity reseed the slip drain defers until this skip has
        // been consumed.  The set is empty unless the toggle is armed ⇒ byte-identical.
        if (ctx.carry.datum_reref_pb_sats.count(parts->satellite)) continue;
        if (harvest_pb_levels) {
            // Guarded: this runs BEFORE the carry_invariant skip below, so it is the
            // one site that queries a phase-bias column for EVERY shared key on a
            // path that otherwise never would.  A key without a column is simply not
            // harvested (a bridge then cannot open on it) — never a thrown stash.
            try {
                const double v =
                    x_s(est.phaseBiasColumn(parts->satellite, parts->signal));
                if (std::isfinite(v))
                    ctx.carry.pb_level_prev[parts->satellite + "_" + parts->signal] = v;
            } catch (...) {}
        }
        // The DD invariant carry is the phase time-link — do NOT also stash the
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
    // §9.7.10: the datum re-reference skip-stash queue is consumed exactly once
    // per KEPT epoch (a withheld epoch never reaches this stash, so the queue —
    // and the deferred ambiguity reseed keyed on its consumption — survives the
    // withhold intact).  Cleared wholesale: a queued satellite with no phase-bias
    // key this epoch had nothing stashed to skip, and prev_carry is rebuilt from
    // scratch above, so there is no stale carry left to guard against.
    if (!ctx.carry.datum_reref_pb_sats.empty()) ctx.carry.datum_reref_pb_sats.clear();
    // Refresh the time-linked previous-value map from THIS kept epoch's
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

    // ---- READMIT continuity (E2 seed-first re-admission + the block re-coupling edge) ----
    // (SPEC_synthetic_minimum_network §18.4; SPEC_amb_iono_split_overcount §7 FIRST.)  Runs
    // AFTER the wholesale rebuild above, so "absent from the new stash" is exact, and on
    // KEPT epochs only — a withheld epoch never reaches here, which is why the detector
    // needs >= kReadmitMinGapEp loop epochs of absence rather than one.
    if (readmit_h > 0.0 || ctx.knobs.readmit_marg_ep > 0) {
        const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        // (1) The re-admission detector + the coupling marginalization window (the block's
        //     DECOUPLING edge, standing invariant #7): a satellite whose clock column is
        //     registered now but was last stashed >= kReadmitMinGapEp loop epochs ago.
        for (const std::string& sat : registered_clock_sats) {
            long long& last = ctx.carry.readmit_last_seen_ep[sat];   // 0 on first sight
            const long long gap_ep = (last > 0) ? ep - last : 0;
            const bool readmitted = last > 0 && gap_ep >= kReadmitMinGapEp;
            last = ep;
            if (!readmitted) continue;
            ++ctx.counters.n_readmit_events;
            std::string marg;
            if (ctx.knobs.readmit_marg_ep > 0) {
                ctx.carry.readmit_marg[sat] = ep + ctx.knobs.readmit_marg_ep;
                ++ctx.counters.n_readmit_marg_windows;
                marg = fmt::format(";marg_until_ep={}", ep + ctx.knobs.readmit_marg_ep);
            }
            traces.emitEvent(t, "READMIT", "readmit", sat, "", static_cast<double>(gap_ep),
                             static_cast<double>(kReadmitMinGapEp), "readmit",
                             fmt::format("gap_ep={}{}", gap_ep, marg));
        }
        // Expired windows: the block's RE-COUPLING edge (the returned satellite's columns
        // fold as PRESENT terms again) — traced per satellite.
        for (auto it = ctx.carry.readmit_marg.begin(); it != ctx.carry.readmit_marg.end();) {
            if (ep >= it->second) {
                traces.emitEvent(t, "READMIT", "marg_expired", it->first, "",
                                 static_cast<double>(it->second), std::nullopt, "recouple",
                                 "coupling terms on this satellite fold as PRESENT again");
                it = ctx.carry.readmit_marg.erase(it);
            } else {
                ++it;
            }
        }
        // (2) Park departures; retire consumed or expired parks (the seed knob only).
        if (readmit_h > 0.0) {
            const double now_s = ctx.carry.readmit_now_s;
            std::set<std::tuple<int, std::string, std::string>> live;
            for (const CarryPrior& cp : prev_carry)
                live.emplace(static_cast<int>(cp.kind), cp.sat, cp.signal);
            std::map<std::string, int> parked_now;
            for (const CarryPrior& oc : old_carry) {
                // The phase-bias frame is re-realized at the cold re-pin (the leveling
                // carries do not survive a 55 min absence at the 120 s leveling gap), and
                // a stiff PB seed would fight that pin — so only Clock + CodeBias park.
                if (oc.kind != SharedParamKind::Clock
                    && !(oc.kind == SharedParamKind::CodeBias && ctx.knobs.readmit_seed_cb))
                    continue;
                if (live.count({static_cast<int>(oc.kind), oc.sat, oc.signal})) continue;
                const bool present = readmitColumnPresent(est, oc);
                if (oc.t_park_s > 0.0) {
                    // Already parked.  Column back but NOT re-stashed (skip-stash, pinned,
                    // non-finite) ⇒ the memory dies with the skip; past the horizon ⇒ drop.
                    if (present) { ++ctx.counters.n_readmit_dropped_stale; continue; }
                    if (now_s - oc.t_park_s > readmit_h) { ++ctx.counters.n_readmit_expired; continue; }
                    prev_carry.push_back(oc);
                    continue;
                }
                // Live last epoch, absent now: a DEPARTURE only if the column itself left
                // the layout (a skip-stash / pinned / non-finite key is not a departure).
                if (present) continue;
                CarryPrior cp = oc;
                cp.t_park_s  = (ctx.carry.readmit_prev_stash_s > 0.0)
                                   ? ctx.carry.readmit_prev_stash_s : now_s;
                cp.inflate_q = false;
                prev_carry.push_back(std::move(cp));
                ++ctx.counters.n_readmit_parked;
                ++parked_now[oc.sat];
            }
            for (const auto& [sat, n] : parked_now)
                traces.emitEvent(t, "READMIT", "park", sat, "", static_cast<double>(n),
                                 readmit_h, "park",
                                 "D2 clock + code-bias carry parked at departure");
            // (3) Seed-vs-solve check: mark-if-moved telemetry for every seeded kind (the
            //     clock's authoritative mark is the Q3 guard's own trip, counted separately).
            for (const auto& sd : ctx.carry.readmit_seed_check) {
                Eigen::Index col = -1;
                try {
                    col = (sd.kind == SharedParamKind::Clock)
                              ? est.clockColumn(sd.sat)
                              : est.codeBiasColumn(sd.sat, sd.signal);
                } catch (...) { continue; }
                if (col < 0 || col >= x_s.size() || col >= cov.rows()) continue;
                const double e = x_s(col) - sd.value;
                const double S = std::max(1e-12, sd.var_eff - cov(col, col));
                const double w = std::abs(e) / std::sqrt(S);
                if (!std::isfinite(w)) continue;
                ++ctx.counters.n_readmit_seed_check;
                const bool out = w > kReadmitSeedK;
                if (out) ++ctx.counters.n_readmit_seed_outlier;
                traces.emitEvent(t, "READMIT", out ? "seed_moved" : "seed_held", sd.sat,
                                 sd.signal, w, kReadmitSeedK, out ? "telemetry" : "ok",
                                 fmt::format("kind={};e={:.4f};sqrtS={:.4f}",
                                             sd.kind == SharedParamKind::Clock ? "clock"
                                                                               : "code_bias",
                                             e, std::sqrt(S)));
            }
            ctx.carry.readmit_seed_check.clear();
            ctx.carry.readmit_prev_stash_s = now_s;
        }
    }
}

} // namespace GnssProcessor
