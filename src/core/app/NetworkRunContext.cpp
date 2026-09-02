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
#include "filter/NetworkPrefitCensus.hpp"   // parseFuseClassMask / fuseClassMaskName (SPEC_prefit_census_observability INC-2)

#include "config/GnssConfig.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkClockAnchor.hpp"      // Card Q3 guarded temporal clock anchor
#include "filter/NetworkPriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkInvariantCarryDriver.hpp"   // RTB-7(c) branch-faithful carry (de)serialize
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
    k.zwd_q = config_.networkZwdCarryQ();
    k.zwd_seam_on = config_.networkZwdSeamReseed();
    k.zwd_seam_epochs = config_.networkZwdSeamReseedEpochs();
    k.amb_seam_reinflate_var = config_.networkAmbSeamReinflateVarM2();
    // Unconditional Z-row capture (SPEC_synthetic_minimum_network §17 finding 3):
    // any snapshot-armed run records + harvests the per-station ZWD posterior so
    // EVERY snapshot carries Z rows — read-only w.r.t. the solve, no inject.
    k.zwd_capture = !nc.state_snapshot_path.empty();
    k.iono_on = config_.networkCarryIono();  // loader guarantees phase_leveling + relevel_free_epoch (scope=relevel) + the SH/mech-B/carry_invariant mutexes
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
    // Card CLK-R (docs/SPEC_clock_emit_rebase.md): clock emission re-base knobs.
    // The engine itself lives in NetworkCarryState (its per-sat baselines and
    // frame accumulators must survive the epoch loop) and is configured by the
    // driver next to the PB-filter setup.
    k.clk_frame_hold = config_.networkEmitClockFrameHold();
    k.clk_jump_test  = config_.networkClockDiscJumpTest();
    // Card Q3 (docs/SPEC_clock_temporal_anchor.md): the guarded temporal clock
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
    // POOL RE-HOST (docs/SPEC_raim_pool_rehost.md).  Resolved HERE, beside the other
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
    // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md).  The loader has
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
        // TWO-EDGE CONTINUOUS HANDOVER (docs/SPEC_pivot_visibility_gauge_defect.md).
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
        // QUEUE item 1(d): the PHASE_ANCHOR carry re-reference — same placement
        // and the same belt-and-braces as the handover knob directly above (it
        // is meaningful only where an anchor pin exists to hand over, and the
        // eq34 chain owns its own eq34_reref carry hygiene).
        k.pa_carry_reref = anchored && !k.phase_datum_eq34
                           && config_.networkPhaseAnchorCarryReref();
    }
    // SBC S-basis completion (network.sbasis_complete).  Resolved here so the
    // pb_level_prev harvest (which the completion's continuity targets re-use —
    // the shared PhaseBias carry is not in prev_carry under carry_invariant) and
    // the solve-glue block read one predicate.
    k.sbasis_complete_on  = config_.networkSbasisComplete();
    k.sbasis_complete_max = config_.networkSbasisCompleteMax();
    // Flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md).  The loader
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
    // Iono-deweight RELEASE hygiene (docs/SPEC_deweight_release_hygiene.md).  The
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
    // Redundancy / testability census (docs/SPEC_redundancy_census.md).  Nothing
    // to resolve against the estimation configuration — the census reads the
    // solved system and writes counters, so the two arming knobs ARE the
    // predicate.  The loader has already clamped the thresholds.
    k.redun_census_on = nc.redundancy_census_stride > 0
                     || nc.redundancy_escalate_sigma > 0.0;
    // Baarda reliability numbers / MDB (docs/SPEC_redundancy_census.md).
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
    // Prior-residual census (docs/SPEC_prior_residual_census.md).  Nothing to
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
        // Constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14).
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
    // BlockFormal LOS block census (docs/SPEC_redundancy_census.md).  The master is
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
    // Station congruence screen (docs/SPEC_redundancy_census.md).  Same host
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
    // flight recorder (docs/SPEC_prefit_census_observability.md).  Nothing to
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
    // Datum graph census (docs/SPEC_datum_graph_census.md).  Nothing to resolve
    // against the estimation configuration: the census reads the epoch's
    // registration sets and the admitted support, and writes nothing back, so
    // the master arm IS the predicate.  Deliberately NOT AND-ed with
    // `sbasis_complete`: without SBC the spectral side still supplies the
    // observed-vs-declared COUNT, the reconciliation degrades to count-level and
    // the trace says so (`obs_src=count`) — silently disarming the census there
    // would hide the tier-1 fragmentation reading, which needs no spectrum at all.
    k.dg_on            = nc.datum_graph_census;
    k.dg_event_holdoff = static_cast<long long>(std::max(0, nc.datum_graph_event_holdoff_ep));
    // Product-referenced measurement admission
    // (docs/SPEC_measurement_admission.md).  Nothing to resolve against the
    // estimation configuration either — the screen reads `observed` / `modeled`
    // and removes observables, so the arming knobs ARE the predicate and the
    // loader has already clamped the thresholds.  `adm_on` is the UNION of the
    // response and the census because both ride the SAME pass; screening implies
    // the census columns (spec §5), which is why `adm_trace` is resolved as the
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
        // Card 3: the Eq-34 chain datum references a different carried-ambiguity
        // frame (per-receiver RPB split + spanning-tree pins) than the anchor/pivot
        // or ensemble datums; a resume across a toggle flip must cold-start.
        // Appended ONLY when armed, so every non-eq34 snapshot stays byte-identical.
        if (phase_datum_eq34) add("phdatum:eq34");
        // Card 4 (M1) OPEN-4: the invariant carry snapshots a DD z-store the legacy
        // carries don't have; a resume across a carry_invariant flip must cold-start.
        // Appended ONLY when armed, so every non-carry snapshot stays byte-identical.
        if (carry_invariant) add("carry:inv");
        // PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md §4): the carried Pᶻ a pd_safe run
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
        // rx-CB carry (SPEC_rx_cb_carry): conditional append ONLY when armed, so every
        // pre-existing snapshot fingerprint is unchanged (the shl2/iono2 pattern).
        if (rx_cb_on) {
            add("rxcb:1"); add(fmt::format("{}", rx_cb_q));
            // Card LV-2 (docs/SPEC_level_information_budget.md): the de-mean changes
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
            // Card S2H (docs/SPEC_level_information_budget.md §17 item 3): the
            // S-consistent harvest re-zeros the carried set's common mode every
            // epoch, so a stored snapshot's absolute rx-CB levels live in a
            // different frame than an untransformed chain's — a resume must not
            // cross the flip.  Same nested conditional-append rule.
            if (ctx.knobs.rx_cb_harvest_demean)
                add(fmt::format("rxcbharv:{}", ctx.knobs.rx_cb_harvest_screen_z));
        }
        // Card Q3 (docs/SPEC_clock_temporal_anchor.md): the anchor changes the TEMPORAL
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
        // M2 v2 (docs/SPEC_carry_vce_apply.md): the variance FACTOR multiplies every
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
        // Card 1f/1g: both change WHICH information an injected coupling row carries —
        // 1f alters the coefficient vector of every truncated row, 1g moves whole columns
        // onto the departed route.  Same footing as carry_cpl_tu above; conditional-append
        // keeps every existing fingerprint byte-identical.
        if (ctx.knobs.carry_cpl_gauge)
            add(fmt::format("carrycplgauge={}", ctx.knobs.carry_cpl_gauge));
        if (ctx.knobs.carry_cpl_reinit)
            add(fmt::format("carrycplreinit={}", ctx.knobs.carry_cpl_reinit));
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
        // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md): DELIBERATELY
        // NOT appended — the exemption record, so a future reader does not "fix" the
        // omission.  eq34_anchor_min_arc_ep / phase_datum_time_linked /
        // eq34_fresh_pin_sigma_scale are a FORWARD-ONLY election and datum-row POLICY:
        // they change which host realizes the gauge from here on, not what any
        // serialized state MEANS (no carried quantity is re-referenced by arming them,
        // and the card's own state — the arc-age map and the open bridges — is not in
        // the snapshot either; the age map re-seeds mature on resume by construction).
        // Same class as adaptive_iono_q, and the same reason: it is what makes an
        // OFF/ON pair legal on ONE shared snapshot, which is the only way the DOY-129
        // 19:26 event can be A/B'd at all (a fingerprinted knob cannot be — see
        // project_warm_resume_card).
        //
        // Flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md): the SAME
        // exemption, recorded for the same reason — the THIRD instance of this
        // precedent, so a future reader does not "fix" the omission.
        // network.signal_integrity.* is a forward-only ROW-WEIGHT and REFLOAT policy:
        // arming it changes which phase rows fold at what weight, and which
        // ambiguities re-tap, from here on.  It re-references no carried quantity and
        // changes what no serialized state MEANS.  Its own state (the per-LOS band
        // history) is not in the snapshot either — a resumed leg's first epoch simply
        // sees every LOS as a first sighting and re-vets it, which is the CONSERVATIVE
        // direction (a resume is exactly a window in which no time-difference detector
        // can operate, so demoting through it is correct rather than merely tolerable).
        // Keeping it out of the fingerprint is what makes the spec §6B/§6D
        // shared-snapshot OFF/ON case-study A/B legal at all.
        //
        // Iono-deweight RELEASE hygiene (docs/SPEC_deweight_release_hygiene.md): the
        // SAME exemption, recorded for the same reason — the FOURTH instance of this
        // precedent, so a future reader does not "fix" the omission.
        // network.iono_dw_{release_reseed,park_bound_tecu,release_validate_k} are a
        // forward-only RELEASE-TIME state and row policy: arming them changes which
        // slant states get rewritten from the field, which ambiguities re-tap, and
        // which tie rows fold, from here on.  They re-reference no carried quantity and
        // change what no serialized state MEANS.  Their own state (the deferred refloat
        // queue, the held-row set, the gate watermark) is not in the snapshot — and
        // neither is the deweight GATE itself, so a resumed leg starts every station at
        // w = 1 with no samples and simply has no release edge to respond to.  That is
        // the honest position, not an oversight: it is exactly why INC-2 carries a
        // gate-cold trigger (spec §6's warm-resume deferral, made explicit).  Keeping
        // all three out of the fingerprint is what makes the spec §5 shared-snapshot
        // OFF/ON A/B on snap_20 legal at all.
        //
        // Product-referenced measurement admission
        // (docs/SPEC_measurement_admission.md): the SAME exemption, recorded for the
        // same reason — the FIFTH instance of this precedent, so a future reader does
        // not "fix" the omission.  network.adm_{screen,bound_m,min_sats,heal_ep,
        // max_frac,trace} are a forward-only ADMISSION policy, RAIM-class: arming
        // them changes which OBSERVABLES enter the model set from here on, and which
        // arcs re-tap.  They re-reference no carried quantity and change what no
        // serialized state MEANS — a carried ambiguity, code bias or slant is the
        // same quantity in the same frame whether or not a screen is deciding what
        // feeds it next epoch.  The card's own state (the per-channel admission
        // records, the event holdoff clock) is not in the snapshot either, and its
        // absence is HANDLED rather than tolerated: `adm_resume_pending` makes the
        // first restored epoch a screened RESTORE rather than a birth, which is the
        // one case the prefit census's seed-birth tripwire structurally cannot see.
        // Keeping the family out of the fingerprint is what makes the spec §7 gates
        // — every one of which replays from a shared `snap_19` / `snap_20` seed with
        // `state_snapshot_require: true` — legal at all.
        //
        // network.adm_revet{,_gap_ep,_ep,_census,_hold} (INC-4, the re-entry vet)
        // join the exemption on exactly the same argument, and it is worth stating
        // for THEM specifically because the vet reads a cross-epoch record and that
        // looks fingerprint-relevant at a glance.  It is not: `vet_left` and
        // `last_ep` live in the same unserialized record map as `admitted`, so a
        // resumed leg starts with no vet windows open and every channel reads as a
        // first sighting — the RESTORE rule's case, already handled.  The vet
        // changes which observables enter the model set going forward and
        // re-references no carried quantity, which is the whole of the RAIM-class
        // test.  It also means an INC-4 OFF/ON pair on ONE snapshot stays A/B-legal,
        // which is precisely how the increment's own field gate is run.
        //
        // QUEUE item 1(d) — network.phase_anchor_carry_reref: the SAME exemption,
        // the SIXTH instance of this precedent.  It is a forward-only CARRY-HYGIENE
        // policy: arming it changes which carried PB priors fold (and which
        // phase-leveling carries survive) on the epochs FOLLOWING a handover, from
        // here on.  It re-references no carried quantity retroactively and changes
        // what no serialized state MEANS — a carried bˢ_σ is the same quantity in
        // the same frame whether or not next epoch's fold will marginalize it.
        // Its own state (phase_anchor_reref) is per-epoch, cleared at each
        // election, never serialized.  Keeping it out of the fingerprint is what
        // makes the 1(d) OFF/ON validation pair on one seed legal at all.
        //
        // SPEC_slip_relevel_walk — network.cycle_slip.relevel_free_epoch /
        // relevel_delta_bridge: the SAME exemption, the SEVENTH instance.  Both
        // are forward-only slip-RESPONSE policies: they change what happens to a
        // leveling carry at the epochs FOLLOWING a detection (skip the fold, or
        // shift the level by the measured float delta), re-reference nothing
        // retroactively, and change what no serialized state MEANS (a snapshot's
        // LocalCarry {value, var} is the same quantity either way).  All of
        // their own state (RelevelEpochState) is one-epoch, never serialized —
        // a resumed leg's first epochs degrade to the pre-fix path, the
        // conservative direction.  ⚠ Do not "fix" these into the fingerprint:
        // that would invalidate every stored seed AND outlaw the same-snapshot
        // OFF/ON A/B their validation depends on.
        //
        // network.zwd_seam_reseed — the EIGHTH instance, a forward-only RESUME
        // response policy (SPEC_synthetic_minimum_network §17 finding 3).  It
        // changes what a resumed run DOES with restored Z rows (inject them for a
        // bounded post-seam window), re-references nothing retroactively, and
        // changes what no serialized state MEANS — a Z row is the same station
        // ZWD posterior whether or not the resuming run folds it.  Its own state
        // (zwd_seam_ep_left) is never serialized: each resume is its own seam.
        // Fingerprinting it would strand every stored snapshot AND outlaw the
        // same-snapshot cold/warm A/B the seam validation is built on.  (zwd_on /
        // zwd_q above stay fingerprinted — the CONTINUOUS carry is solve-affecting
        // on both sides of a seam.)  The unconditional Z-row CAPTURE needs no
        // token either: it adds rows an old reader already parses and changes no
        // solve byte; a resume of a capture-era snapshot under zwd_seam_reseed
        // OFF restores the rows into a map nothing reads.
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
    // M2 v2 (docs/SPEC_carry_vce_apply.md §8) — the ONE field-BLOCKING deferral,
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
                        zwd_carry[idx] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
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
                            // PD-safe INC-2c (docs/SPEC_pd_safe_dd_fold.md §5c): the
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
    // M2 v2 (docs/SPEC_carry_vce_apply.md §2) — the LASER 5-95 variance factor,
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
    // Card Q3 (docs/SPEC_clock_temporal_anchor.md): the guarded temporal clock
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
            // A clock-monitor-flagged satellite folds with the large REINIT variance (a soft
            // large-q reinitialization): the weak prior still anchors the parameter at its
            // previous value — curbing the collective datum drift a hard refloat causes — but
            // is loose enough for the data to walk a frozen clock back toward truth.
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * std::max(0.0, dt);
            // Card Q3 (docs/SPEC_clock_temporal_anchor.md): the satellite clock's
            // temporal constraint.  An inflate_q key is deliberately EXCLUDED — that
            // is a monitor-driven soft refloat, not the hand carry model, and
            // overriding it with a stiff anchor would re-freeze exactly what the
            // monitor just loosened.  OFF ⇒ the returned var_eff is the expression
            // above, character for character.
            if (cta_on && cp.kind == SharedParamKind::Clock && !cp.inflate_q)
                var_eff = clockAnchorFoldVariance(ctx, cp.sat, cp.var, dt,
                                                  cta_now_s, cp.value).var_eff;
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
            if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
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
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * std::max(0.0, dt);
            // Card Q3: the SAME clock-anchor decision as the diagonal branch above.
            // It must be the same, and it must be on the DIAGONAL of cov_eff: the
            // marginal prior on any one state has to be identical on both branches
            // (the property that makes the guard statistic path-independent), and
            // the dense fold's off-diagonals are retained deliberately — the anchor
            // does not re-create the independence assumption C1 removed on the very
            // state it makes stiffest (SPEC §2.1, assessment deviation #1).
            if (cta_on && cp.kind == SharedParamKind::Clock && !cp.inflate_q)
                var_eff = clockAnchorFoldVariance(ctx, cp.sat, cp.var, dt,
                                                  cta_now_s, cp.value).var_eff;
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
    const bool   zwd_on = ctx.knobs.zwd_on;
    const bool   iono_on = ctx.knobs.iono_on;             // QUEUE 8b
    const double iono_q = ctx.knobs.iono_q;
    const double iono_var_floor = ctx.knobs.iono_var_floor;
    const double iono_max_gap_s = ctx.knobs.iono_max_gap_s;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // PRIOR-RESIDUAL CENSUS (docs/SPEC_prior_residual_census.md) — the pl / zwd /
    // rxcb feed arms.  Resolved once at run start into per-family bools, so an
    // OFF run pays one bool test per harvest loop and nothing else.
    const bool   prires_pl   = ctx.knobs.prires_pl;
    const bool   prires_zwd  = ctx.knobs.prires_zwd;
    const bool   prires_iono = ctx.knobs.prires_iono;   // QUEUE 8b
    const bool   prires_rxcb = ctx.knobs.prires_rxcb;
    const double zwd_q       = ctx.knobs.zwd_q;
    // Seam-scoped ZWD re-seed + unconditional Z-row capture (§17 finding 3).
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;

    // --- TWO-EDGE CONTINUOUS HANDOVER: ledger harvest -------------------------
    // (docs/SPEC_pivot_visibility_gauge_defect.md §5.1)
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
                // (invariant #7).  BORDER-EXACT flavor (§5.09.1), deliberately NOT
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
                ctx.carry.handover.value_abs[{static_cast<int>(i),
                                              pa.sat + "_" + pa.signal}] =
                        AnchorHandoverLedger::Entry{value_abs, var};
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
        // ---- Rung 3 v3 lane observatory: PRE-event snapshot -----------------
        // (SPEC_lane_slip_response §7.3.)  Taken here because the loop below
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
                // PRIOR-RESIDUAL CENSUS (docs/SPEC_prior_residual_census.md §6):
                // the SAME (prior − solved, gap, post_var, fold_var) tuple the
                // VCE monitor folds, but WITHOUT its two exclusions — those become
                // the RESPONDED / FRAME labels, which is exactly the population a
                // detector exists to find and a variance component is right to
                // drop.  Hoisted above the monitor so both read one qualification.
                //
                // `folded` is REPRODUCED from the inject predicate rather than
                // mirrored by hand: every clause of the PASS-B injector
                // (ApplicationNetworkAssembly.cpp, the `local_carry.find` block)
                // is a pure function of values still in scope here — the carry
                // entry, the gap, the pending-reseed mark (cleared only AFTER
                // this point), pa.x_lin and var_eff.  Getting this bit wrong is
                // not a cosmetic error: the folded branch on an unfolded row
                // inflates |t| by ~x3000 through the ε clamp and would make the
                // §9.3 positive control pass vacuously.  Two clauses are NOT
                // reproducible here and are assumed to have held, because the
                // ambiguity reached R.pl_amb at all: the gauge-pin exclusion
                // (pinned keys never enter R.pl_amb) and the h2local column
                // lookup.  Note eq34_reref is a FRAME LABEL for pl and NOT a
                // folded=false condition — unlike the shared PB carry, the pl
                // injector has no eq34 skip, so a re-referenced arc really did
                // fold.  Truth wins over symmetry here.
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
                                    if (pl_var_floor > 0.0) v = std::max(v, pl_var_floor);
                                    return v;
                                }();
                            // M2 v2 (docs/SPEC_carry_vce_apply.md): the PASS-B injector
                            // multiplies the SAME hand value by the SAME α² (the factor
                            // table is refreshed once per epoch, at the end, so inject and
                            // harvest read one value), so `fold_var` here is the variance
                            // the fold ACTUALLY used.  Both consumers want that: the census
                            // standardizes by the used weight (§3), and the monitor's
                            // sigma_hat_post readout is only "1 at convergence" if its
                            // denominator is the used weight too.  The HAND value goes in
                            // alongside it because it is the factor iteration's own
                            // denominator and must stay α²-independent.  Unity when OFF.
                            const double fold_var =
                                fold_var_hand * ctx.carry.vce_apply.factor(
                                                    NetworkCarryVceApply::kPl, pa.sat);
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
        // ---- Rung 3 v3 lane observatory: POST-event measurement -------------
        // (SPEC_lane_slip_response §7.3.)  Runs after the harvest loop, so both
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

        // Carry-retirement census (SPEC_withhold_era_release §8): this prune is
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
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, R.pl_zwd.col);
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
                zwd_carry[static_cast<int>(i)] =
                    LocalCarry{value_abs, var, pl_now_s, std::move(coupling)};
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

    // --- RXCB-DATUM gauge census (docs/SPEC_rx_cb_carry.md) ------------------
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

    // --- rx-CB carry (docs/SPEC_rx_cb_carry.md): harvest + stash + prune ------
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
                            // LV-4: mirror the injector's actual configuration, or
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
                // --- Card LV-4: the FRAME-INVARIANT event screen (§11) -------------
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
        // a feedback delay, so under card LV-5 (§12) μ is instead reduced BEFORE
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
            // Card LV-5 (§12): with the within-epoch reduction armed, THIS pass is a
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
    // eq34 reattach hygiene INC-2 (docs/SPEC_eq34_reattach_hygiene.md): refresh the
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
    // §9.7.10: the datum re-reference skip-stash queue is consumed exactly once
    // per KEPT epoch (a withheld epoch never reaches this stash, so the queue —
    // and the deferred ambiguity reseed keyed on its consumption — survives the
    // withhold intact).  Cleared wholesale: a queued satellite with no phase-bias
    // key this epoch had nothing stashed to skip, and prev_carry is rebuilt from
    // scratch above, so there is no stale carry left to guard against.
    if (!ctx.carry.datum_reref_pb_sats.empty()) ctx.carry.datum_reref_pb_sats.clear();
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
