/**
 * @file NetworkRunContext.cpp
 * @brief @internal Knob snapshot, snapshot fingerprint (RTB-7), warm-resume
 *        restore, state-snapshot writer and the carry-lifecycle functions of
 *        Application::runNetwork.
 *
 * The bodies were relocated from ApplicationNetwork.cpp (decomposition INC-4).
 * Each function opens with an alias block that binds the driver's former local
 * names to members of the context, so the relocated code keeps those names.
 */

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // parseFuseClassMask / fuseClassMaskName (SPEC_prefit_census_observability INC-2)

#include "config/GnssConfig.hpp"
#include "common/GnssIdentity.hpp"         // systemOfSat, letterOfSat, constellationKey, SignalId
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "filter/NetworkAmbStateTrace.hpp"   // AmbStateRow / ambStateNoteErase (RF-1 §7.4)
#include "filter/NetworkAmbFloat.hpp"       // ambFloatBirthFloor (RF-1 step 4)
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkClockAnchor.hpp"      // guarded temporal clock anchor (card Q3)
#include "filter/NetworkCarryDisposition.hpp" // the decided-once fold disposition (card PRM phase 1b)
#include "filter/NetworkPriorResidual.hpp"    // prior-residual census (SPEC_prior_residual_census)
#include "filter/NetworkInvariantCarryDriver.hpp"   // branch-faithful carry (de)serialization (RTB-7(c))
#include "products/NetworkClockEmitRebase.hpp"       // CLK-R held frame, restored on warm resume (§18.10)
#include "products/NetworkEmitRebase.hpp"            // eq34 emit re-base frame, restored on warm resume (RS-5, §21.10)
#include "products/NetworkPbEmitFilter.hpp"          // PB emit filter state, restored on warm resume (RS-5, §21.10)
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkPivotHandover.hpp"   // datumChangeAffectsKey (handover scope predicate)
#include "filter/NetworkSbasis.hpp"          // kWindUpYawModelId (wind-up model revision token)
#include "filter/StateKeySchema.hpp"         // StateKeys::parseSatPhaseBiasKey (harvest/stash)
#include "math/TimeUtils.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"
#include "realtime/RealTimeEngine.hpp"       // consumer snapshot written beside the network history copy (card CC, part B)
#include "rinex/MultiRinexSync.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace GnssProcessor {

NetworkKnobs NetworkKnobs::snapshot(const GnssConfig& config_, const NetworkConfig& nc) {
    NetworkKnobs k;
    k.q_clock = config_.getProcessNoise().sat_clock;
    k.q_pb = config_.getProcessNoise().sat_phase_bias;
    k.q_cb = config_.getProcessNoise().sat_code_bias;
    k.ledger_reentry = config_.networkLedgerReentry();
    k.ledger_screens = config_.networkLedgerScreens();
    k.ledger_anchor  = config_.networkLedgerAnchor();
    k.pl_on = config_.networkPhaseLeveling();
    k.pl_q_amb = config_.networkPhaseLevelingQAmb();
    k.pl_warmup = config_.networkPhaseLevelingWarmupEpochs();
    k.pl_max_gap_s = config_.networkPhaseLevelingMaxGapS();
    k.pl_var_floor = config_.networkPhaseLevelingVarFloorM2();
    k.pl_los_scale = config_.networkPhaseLevelingLosScale();
    // Under the float_kf vehicle the station-wide jointly whitened local block is part of
    // the vehicle, not a knob: `buildLocalBlock` is the vehicle's definition of the fold.
    // pl_local_block is therefore the resolved predicate (F3 knob or vehicle), and every
    // downstream consumer (the PASS-B inject, the harvest's cross-covariance stash, the
    // run-end readout, the trace's `route`) follows the vehicle with no further branch.
    // The loader forces the F3 knob itself off under float_kf, so one switch controls the
    // fold.  The vehicle is read from the config, not from `k`: the amb_float_* members
    // are filled below, and reading them here would leave the block disarmed without a
    // warning.  See docs/refound/RF-1_float_ambiguities_design.md §3.2, §7.2 (RF-1 step 3).
    k.pl_blk_from_vehicle = config_.getNetworkConfig().ambiguity_vehicle
                            == NetworkAmbiguityVehicle::FloatKf;
    k.pl_local_block = config_.networkPhaseLevelingLocalBlock() || k.pl_blk_from_vehicle;
    k.pl_blk_min_age_ep = config_.networkPhaseLevelingLocalBlockMinAgeEp();   // admission age gate of the F3 block
    k.pl_blk_young_free = config_.networkPhaseLevelingLocalBlockYoungMode() == "free";
    // Ambiguity vehicle and its supporting knobs (RF-1 §7.1,
    // docs/refound/RF-1_float_ambiguities_design.md).  The joint block (step 3) follows the
    // vehicle (above); the birth variance and the drop (step 4) and the declared anchor
    // S-transform (step 5) each have their own knob below.
    k.amb_float_kf = config_.getNetworkConfig().ambiguity_vehicle
                     == NetworkAmbiguityVehicle::FloatKf;
    k.amb_birth_var_floor = config_.getNetworkConfig().amb_float_birth_var_m2;
    // Variance floor applied at the drop only (RF-3a).  It is gated on the vehicle here, so
    // every downstream site reads one number and a floor configured without float_kf
    // resolves to 0 instead of acting at some sites and not at others.
    k.amb_drop_decouple_always = k.amb_float_kf && config_.networkAmbFloatDropDecoupleAlways();   // card CTU section 14
    // Card LGM: the loader has validated the posture, so the knob is read as is.
    k.pc_on        = nc.phase_colour_state;
    k.pc_sigma_zen = nc.phase_colour_sigma_zen_m;
    k.pc_tau_s     = nc.phase_colour_tau_s;
    k.pc_floor_deg = nc.phase_colour_elev_floor_deg;
    k.pc_max_gap_s = nc.phase_colour_max_gap_s;
    k.pc_trace     = nc.phase_colour_state && nc.trace_phase_colour;
    k.amb_if_trace = nc.trace_amb_if;   // card AIL
    k.amb_drop_var_floor  = k.amb_float_kf
                                ? config_.getNetworkConfig().amb_float_drop_var_m2 : 0.0;
    // The drop floor's warm-up gate (RF-3c) has no meaning without the floor, so it
    // resolves to 0 when the floor is off; a site reads the pair as "floor, from epoch N".
    k.amb_drop_floor_after_ep =
        (k.amb_drop_var_floor > 0.0)
            ? config_.getNetworkConfig().amb_float_drop_floor_after_ep : 0;
    k.amb_float_q_m2s = config_.getNetworkConfig().amb_float_q_m2s;
    k.amb_float_declared_handover = config_.getNetworkConfig().amb_float_declared_handover;
    // Held S-basis (RF-2 item 1).  The loader already interlocks it with float_kf and
    // phase_datum_prioritized; the vehicle test is repeated here so that a later loader
    // edit cannot arm it on the level vehicle through this path.
    k.pd_held_basis = config_.networkPhaseDatumHeldBasis()
                      && config_.getNetworkConfig().ambiguity_vehicle
                             == NetworkAmbiguityVehicle::FloatKf;
    k.pd_held_cold_release = k.pd_held_basis && config_.networkPhaseDatumHeldColdRelease();
    k.pd_held_mark_cold    = k.pd_held_basis && config_.networkPhaseDatumHeldMarkCold();
    k.pa_min_age_ep    = config_.networkPhaseAnchorMinAgeEp();
    k.pa_flag_quiet_ep = config_.networkPhaseAnchorFlagQuietEp();
    k.trace_amb_state = config_.getNetworkConfig().trace_amb_state;
    k.trace_amb_state_every =
        std::max(1, config_.getNetworkConfig().trace_amb_state_every);
    // Age-aware young-arc inflation (SPEC_amb_iono_split_overcount §6.17–§6.18).  The
    // loader already disables it (V0 -> 0) off the leveling vehicle, so Knobs::on() alone
    // is the live predicate.
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
    k.carry_cpl_exact = config_.networkCarryCouplingExactUpdate();   // card CTU; loader guarantees tu + fullcov + !joint
    k.carry_cpl_exact_cross = config_.networkCarryCouplingExactCross();   // card WSX; loader guarantees exact
    k.pfq_on = config_.networkClockPivotFrameQ();   // card PFQ; loader guarantees fullcov + the pin datum
    k.carry_cpl_clone = config_.networkCarryCouplingClockClone();   // card SCL; loader guarantees exact + fullcov
    k.carry_cpl_shx   = config_.networkCarryCouplingSharedHistory();       // card SCL2 X2; loader guarantees B1
    k.shx_rank        = config_.networkCarryCouplingSharedHistoryRank();   // card SCL2 X2; loader guarantees [1, 8]
    k.carry_cpl_bclone = config_.networkCarryCouplingBiasClone();       // card BCL-a; loader guarantees B1
    k.carry_cpl_dclone = config_.networkCarryCouplingDepartedClone();   // card BCL-d; loader guarantees B1
    k.clm_on    = config_.networkClockLevelMonitor();      // card CTU monitor (trace class)
    k.clm_raise = config_.networkClockLevelBoundRaise();   // card CTU raise; loader guarantees clm_on
    k.clm_innov_window_ep = config_.networkClockLevelInnovWindowEp();   // card CTU section 14 instrument window
    k.pv_on        = config_.networkPhaseVisibleMonitor();     // card PVN (trace class)
    k.nbp_on       = config_.networkAprioriBrdcClock();        // card NBP (fingerprinted below)
    k.pv_window_ep = config_.networkPhaseVisibleWindowEp();
    k.clm_restore_credit_ep = config_.networkClockLevelRestoreCreditEp();   // card CTU G5
    k.clm_restore_raise = config_.networkClockLevelRestoreRaise();       // card CTU G5; loader guarantees clm_on
    k.carry_cpl_shmarg = config_.networkCarryCouplingShMarginalize();
    k.carry_cpl_gauge  = config_.networkCarryCouplingGaugeProject();
    k.carry_cpl_reinit = config_.networkCarryCouplingReinitSources();
    k.zwd_on = config_.networkCarryZwd();
    k.zwd_in_block = config_.networkCarryZwdInBlock();  // B9-in-block; loader guarantees zwd_on + pl_local_block
    k.zwd_q = config_.networkZwdCarryQ();
    k.zwd_seam_on = config_.networkZwdSeamReseed();
    k.zwd_seam_epochs = config_.networkZwdSeamReseedEpochs();
    k.zwd_seam_q = config_.networkZwdSeamReseedQ();
    k.amb_seam_reinflate_var = config_.networkAmbSeamReinflateVarM2();
    k.resume_frame_restore   = config_.networkResumeFrameRestore();      // frame state (§18.10)
    k.resume_coupling_restore = config_.networkResumeCouplingRestore();  // coupling rows (§18.12)
    k.resume_datum_restore   = config_.networkResumeDatumRestore();      // datum tree (RS-3, §21.7)
    k.resume_emit_restore    = config_.networkResumeEmitRestore();       // emit engines (RS-5, §21.10)
    k.resume_station_restore = config_.networkResumeStationRestore();    // station layer (RS-6, §21.13)
    k.resume_station_rxcb    = config_.networkResumeStationRestoreRxCb();
    k.resume_station_shdev   = config_.networkResumeStationRestoreShDev();
    k.resume_signal_canon_restore = config_.networkResumeSignalCanonRestore();  // signal latch (RS-8, §21.16)
    k.resume_border_restore  = config_.networkResumeBorderRestore();     // border covariance (RS-7, §21.14)
    k.resume_rxcb_lin_restore = config_.networkResumeRxCbLinRestore();   // rx-CB x_lin (RS-9, §21.18)
    k.resume_pb_seam_mark    = config_.networkResumePbSeamMark();       // PB seam mark (RS-4, §21.9)
    k.resume_slip_warmup_ep  = config_.networkResumeSlipWarmupEp();      // MW/LLI warm-up (§18.10)
    k.resume_d5_warmup_ep    = config_.networkResumeD5WarmupEp();        // warm-up of the D5 gate (RS-2b, §21.6)
    k.readmit_seed_horizon_s = config_.networkReadmitSeedHorizonS();     // seed-first re-admission (E2)
    k.readmit_marg_ep        = config_.networkReadmitCouplingMargEp();   // block/row re-coupling edge
    k.readmit_seed_cb        = config_.networkReadmitSeedCodeBias();     // seed the code biases too (or clock only)
    // Z-row capture is unconditional (SPEC_synthetic_minimum_network §17 finding 3): a run
    // with a snapshot path records and harvests the per-station ZWD posterior, so every
    // snapshot carries Z rows.  The capture is read-only with respect to the solve (no
    // inject).
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
    k.rxcb_blk = config_.networkCarryRxCodeBiasInBlock();   // RXCB-IN-BLOCK; loader guarantees rx_cb_on + coupling + a local block
    k.rx_cb_datum = config_.networkRxCodeBiasDatum();
    k.rx_cb_datum_sigma = config_.networkRxCodeBiasDatumSigmaM();
    k.sta_prior_pre_pd = config_.getNetworkConfig().station_prior_before_pd;
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
    k.vce_apply_on = config_.networkCarryVceApply();   // VCE apply (M2 v2); loader guarantees carry_vce
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
    k.handover_stransform_before_rows = config_.networkHandoverStransformBeforeRows();
    k.handover_stransform_dense = config_.networkHandoverStransformDense();   // card SCL B2
    k.rebase_complete = config_.networkCarryRebaseComplete();   // card PRM phase 3b step 2
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
    // Clock emission re-base knobs (card CLK-R).  The engine lives in NetworkCarryState,
    // because its per-satellite baselines and frame accumulators outlive the epoch, and
    // the driver configures it next to the PB-filter setup.
    k.clk_frame_hold = config_.networkEmitClockFrameHold();
    k.clk_jump_test  = config_.networkClockDiscJumpTest();
    // Emit-side phase-bias hold (card PBH).  The held values and generations live in
    // NetworkCarryState because they outlive the epoch; these are the knobs the
    // NetworkPbHold seam reads.
    k.emit_pb_hold           = config_.getNetworkConfig().emit_pb_hold;
    k.emit_pb_hold_max_gap_s = config_.getNetworkConfig().emit_pb_hold_max_gap_s;
    k.emit_pb_hold_phase_clock = config_.getNetworkConfig().emit_pb_hold_phase_clock;   // card LAH
    k.emit_pb_hold_smoothed    = config_.getNetworkConfig().emit_pb_hold_smoothed;      // card SMH
    k.emit_pb_hold_smoothed_block = config_.getNetworkConfig().emit_pb_hold_smoothed_block;   // card SMD
    // Guarded temporal clock anchor.  `cta_on` is resolved against the clock_model
    // interlock here, so every downstream site reads one predicate.  Under the two-state
    // oscillator the clock is never stashed to the D2 carry, so the anchor has nothing to
    // fold and reads as off (the loader reports an error on the combination).
    k.clk_model_on = config_.networkClockModel();
    k.cta_on             = config_.networkClockTemporalAnchor() && !k.clk_model_on;
    k.cta_datum_keep     = config_.networkClockAnchorDatumKeep() && k.cta_on;   // card SCL B2
    k.cta_q              = config_.networkClockAnchorQ();
    k.cta_var_floor      = config_.networkClockAnchorVarFloorM2();
    k.cta_max_gap_s      = config_.networkClockAnchorMaxGapS();
    k.cta_k              = config_.networkClockAnchorK();
    k.cta_abs_m          = config_.networkClockAnchorAbsM();
    k.cta_sigma_floor_m  = config_.networkClockAnchorSigmaFloorM();
    k.cta_space          = config_.networkClockAnchorGuardSpace();          // card CTU section 14
    k.cta_phi_floor_m    = config_.networkClockAnchorPhaseSigmaFloorM();
    k.cta_recouple_ep    = config_.networkClockAnchorRecoupleEpochs();
    k.cta_mark           = config_.networkClockAnchorMark();
    k.cta_resolve        = config_.networkClockAnchorResolve();
    k.cta_gauge_free     = config_.networkClockAnchorGaugeFree();
    // Code-only clock level re-base (card CLV).  Resolved against the clock_model
    // interlock: under the two-state oscillator the clock lives on clock_osc, which the
    // re-base does not move, so the carried clock and its oscillator would diverge.
    k.clv_on           = config_.networkClockLevelCarry() && !k.clk_model_on;
    k.clv_floor_m      = config_.networkClockLevelSigmaFloorM();
    k.clv_horizon_s    = config_.networkClockLevelHorizonS();
    k.clv_ignition_var = config_.getInitialStd().sat_clock * config_.getInitialStd().sat_clock;
    k.clv_pin_pair_read  = config_.networkClockLevelPinPairRead();
    k.clv_cb_follow      = config_.networkClockLevelFreeCbFollow();
    k.clv_datum_pause    = config_.networkClockLevelDatumPause();
    k.clv_confirm_epochs = std::max(1, config_.networkClockLevelConfirmEpochs());
    k.clv_pivot_ref      = config_.networkClockLevelPivotRef();
    k.clv_free_cb_rebase = config_.networkClockLevelFreeCbRebase();
    // Card HLT: broadcast satellite health on the network path.
    k.sat_health         = config_.networkSatHealth();
    // Gauge-free carry downdate: one resolved predicate that every fold, withdraw and
    // replay site reads (card C-GF).
    //
    // The downdate belongs to the carry fold, not to the Q3 clock anchor: it removes the
    // datum's own direction from the carried information, which is a property of carrying
    // information across a re-realizable datum, not of anchoring a clock in time.  It is
    // armed either through `clock_anchor_gauge_free` with the Q3 anchor on, or through the
    // dense carry (network.shared_carry_full_cov) with network.shared_carry_gauge_free.
    //
    // It is a single resolved field, not a predicate re-derived per site, because fold and
    // withdraw have to agree: the withdraw subtracts what the fold added, and a fold that
    // downdated paired with a withdraw that did not leaves a residue −ΛU(UᵀΛU)⁻¹UᵀΛ, which
    // is negative semidefinite.  The reduced normals then lose PSD along the gauge, and the
    // constrained solve either throws on its pivot guard (the epoch is skipped, every
    // epoch) or factorizes a saddle and returns a wrong x_s without an error.
    k.carry_gauge_free   = (k.cta_on && k.cta_gauge_free)
                        || (config_.networkSharedCarryFullCov()
                            && config_.networkSharedCarryGaugeFree());
    // The closure test belongs to the dense-carry arming only: the Q3 path keeps its
    // original rule, so Q3 configurations stay byte-identical and an A/B against them
    // isolates the closure test as the one changed variable.
    k.carry_gauge_free_closure = config_.networkSharedCarryFullCov()
                              && config_.networkSharedCarryGaugeFree();
    // The denominator of the anchor guard and its census is resolved here, once (card Q3
    // §2.1), for the same reason `cta_on` resolves its interlock here: the guard and the
    // census have to standardize identically, or the census stops being a cross-check on
    // the guard, and a mapping derived separately at two call sites can drift apart.
    // `auto` follows the fold, which is the behavior before this knob existed, so `auto`
    // is byte-identical to it.
    {
        const auto stat = config_.networkClockAnchorGaugeFreeStat();
        k.cta_stat_proj_post  = (stat == NetworkClockAnchorStat::Post)
                             || (stat == NetworkClockAnchorStat::Proj)
                             // `auto` follows the fold, and the fold is `carry_gauge_free`
                             // (card C-GF).  The term is OR-ed with the original one rather
                             // than replacing it, so a config with cta_gauge_free set and
                             // cta_on false keeps its denominator bit for bit (the census
                             // reads the same predicates) while the carry-side arming is
                             // still followed.
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
    // RAIM pool re-host (network.raim_carry_invariant).  Resolved here, beside the other
    // carry_invariant-derived knobs rather than with the raim_* family, so that one bool
    // tells all three consumers whether the re-hosted census is live: the driver's
    // call-site gate, the assembly's census fill and the seam's pool selector.  They
    // cannot then disagree about whether qc_amb exists.
    //
    // The k.carry_invariant conjunct also includes phase_datum_eq34 (above).  That is
    // defensive redundancy, not a live discriminator: carry_invariant without eq34 is a
    // load error (the loader's "network.carry_invariant requires network.phase_datum_eq34"
    // check in GnssConfigLoadNetwork.cpp returns false), so no config that could exercise
    // the difference reaches this line.  It is kept because it matches how
    // carry_invariant_pd_safe and carry_aware_covariance resolve, and it stays correct if
    // that requirement is relaxed.
    k.raim_ci_on = k.raim_on && k.carry_invariant && config_.networkRaimCarryInvariant();
    // Gross line-of-sight screen (card GL, docs/SPEC_event_harness.md §24.18).  Resolved
    // once here so the driver, the assembly, the seam and the teardown read one bool.  It
    // has no interlock: the screen owns its residual walk and its state, so it runs with no
    // other tier armed, which matters because every other post-fit tier is phase-only.
    k.gl_on      = config_.getNetworkConfig().gross_los_screen;
    k.gl_deweight = k.gl_on ? config_.getNetworkConfig().gross_los_deweight : 1.0;
    // Satellite stand-down (GL-2, §24.19), gated on gl_on here so every downstream site
    // reads one bool.  The satellite rule sets the scope of the gross-LOS rule (it tallies
    // the same over-bar verdicts), so it has no meaning without the screen.
    k.gl2_on = k.gl_on && config_.getNetworkConfig().gross_los_sat_standdown;
    // The four sub-rules of RF-3b, each gated on the screen for the same reason as GL-2.
    k.gl_min_rows_available = k.gl_on
        && config_.getNetworkConfig().gross_los_min_rows_rule
               == NetworkGrossLosMinRowsRule::Available;
    k.gl_bound         = k.gl_on && config_.getNetworkConfig().gross_los_bound;
    k.gl_emit_exclude  = k.gl_on && config_.getNetworkConfig().gross_los_emit_exclude;
    k.gl_station_tier  = k.gl_on && config_.getNetworkConfig().gross_los_station_tier;
    k.raim_ci_refloat = k.raim_ci_on && config_.networkRaimCarryInvariantRefloat();
    // Card RPA: the RAIM pools restricted to observation phase rows.  ANDed with raim_on
    // (the loader already guarantees it), so both pool walks and the run-end lines read
    // one bool.  Not appended to the snapshot fingerprint below: a QC knob, like raim_*.
    k.raim_pool_data_rows_only = k.raim_on && config_.networkRaimPoolDataRowsOnly();
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
    // Resolved once for the run (§9.7.10): the datum-station slip path exists only under
    // the legacy pivot datum.  Every no-pivot datum already runs the slip loops from
    // i_lo = 0, so station 0 has ordinary coverage there.
    k.datum_station_detect = config_.networkCycleSlipDatumStationDetect()
                             && !k.phase_datum_prioritized;
    k.phase_hold_sigma = k.phase_datum_ensemble ? config_.networkPhaseDatumHoldSigmaM() : 0.0;
    k.phase_hold_on = k.phase_hold_sigma > 0.0;
    k.phase_hold_var = k.phase_hold_sigma * k.phase_hold_sigma;
    // eq34 reattach hygiene.  The loader already disarms every increment outside an
    // anchored election; the `&& !ensemble` here is a second guard, so a hand-built
    // NetworkConfig (tests) cannot arm the seam on a datum that elects no anchor.
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
        // Two-edge continuous handover.  Resolved here, inside `anchored`, because it
        // applies only where an anchor pin exists to hand over.  `!phase_datum_eq34`
        // narrows it further: the chain datum owns its pin-target policy (pin-round /
        // seed-round), and the two would compete for the same target.  The loader already
        // disarms all three cases; this is the same second guard as the block above, so a
        // hand-built NetworkConfig in a test cannot arm it on the wrong election.
        k.pd_handover_continuous = anchored && !k.phase_datum_eq34
                                   && config_.networkPhaseDatumHandoverContinuous();
        k.pd_handover_max_var = k.pd_handover_continuous
                                ? config_.networkPhaseDatumHandoverMaxVarM2() : 0.0;
        k.pd_handover_max_var_ratio = k.pd_handover_continuous
                                ? config_.networkPhaseDatumHandoverMaxVarRatio() : 0.0;
        // PHASE_ANCHOR carry re-reference (queue item 1(d)), with the same placement and
        // the same second guard as the handover knob above: it applies only where an
        // anchor pin exists to hand over, and the eq34 chain has its own eq34_reref carry
        // hygiene.
        k.pa_carry_reref = anchored && !k.phase_datum_eq34
                           && config_.networkPhaseAnchorCarryReref();
        // Cold anchor fallback (SPEC_event_harness §15).  Resolved against
        // pd_handover_continuous, not against `anchored`, because the pass screens
        // candidates with the handover ledger's usability predicate; without the ledger
        // it has nothing to read and would elect by coverage rank, as it does when off.
        // The loader warns about that inert case; this is the second guard a hand-built
        // NetworkConfig (tests) needs.
        k.pd_candidacy_usable = k.pd_handover_continuous
                                && config_.networkPhaseDatumCandidacyUsable();
        k.pd_candidacy_min_age_ep = k.pd_candidacy_usable
                                ? config_.networkPhaseDatumCandidacyMinAgeEp() : 0;
        // Satellite clause (iteration 2), resolved with its parent so every site reads one
        // bool and an A/B against iteration 1 changes a single knob.
        k.pd_candidacy_satellite = k.pd_candidacy_usable
                                && config_.networkPhaseDatumCandidacySatellite();
    }
    // S-basis completion (network.sbasis_complete, SBC).  Resolved here so that the
    // pb_level_prev harvest and the solve-glue block read one predicate.  The completion's
    // continuity targets reuse that harvest, because under carry_invariant the shared
    // PhaseBias carry is not in prev_carry.
    k.sbasis_complete_on  = config_.networkSbasisComplete();
    k.sbasis_complete_max = config_.networkSbasisCompleteMax();
    // Flicker/gap signal integrity.  The loader already disarms every action when the
    // census master is off; resolving them here as well is a second guard, so a
    // hand-built NetworkConfig (tests) cannot arm a demotion behind a classifier that
    // never runs.  `sig_withhold` covers both spellings of the withhold disposition (the
    // explicit bool and the spec's "scale <= 0"), so downstream sites read one field.
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
        // The resume warm-up has an effect only when there is a demotion to apply.
        k.sig_resume_warmup   = si.census && si.resume_warmup_vet
                                && (k.sig_withhold || k.sig_unvetted_scale > 1.0);
    }
    // Iono-deweight release hygiene.  The loader already disarms every increment without
    // the SH field (and INC-1 without the adaptive gate, the only source of a release
    // edge); resolving it here as well is a second guard, so a hand-built NetworkConfig
    // (tests) cannot arm a response to an event that cannot occur.  `dw_hygiene_on` is the
    // single predicate the seam and the assembly's held-row lookup read.
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
    // Redundancy / testability census.  Nothing to resolve against the estimation
    // configuration: the census reads the solved system and writes counters, so the two
    // arming knobs are the predicate.  The loader already clamps the thresholds.
    k.redun_census_on = nc.redundancy_census_stride > 0
                     || nc.redundancy_escalate_sigma > 0.0;
    // Baarda reliability numbers / MDB.  Nothing to resolve against the estimation
    // configuration either (MDB reads the (sigma_i, r_i) pairs the host loops already
    // hold and writes counters), but the two source flags are resolved here rather than
    // at the feed sites, for two reasons.  (1) The RAIM source needs its host tier live:
    // raim_redundancy_normalize alone computes no r_i, because networkPostFitRaim is not
    // called without network.raim.  (2) The run-end lines for invariant #4 report which
    // feed was armed when a source contributed zero rows, since "the RAIM pool never ran
    // warm" and "no sweep completed" need different fixes.  Requiring a live source is a
    // second guard behind the loader's error, so a hand-built NetworkConfig (tests)
    // cannot arm a flush over wiring that never runs.
    k.mdb_from_raim   = config_.networkRaim() && config_.networkRaimRedundancyNormalize();
    k.mdb_from_census = k.redun_census_on;
    k.mdb_on          = nc.mdb && (k.mdb_from_raim || k.mdb_from_census);
    k.mdb_event_m     = std::max(0.0, nc.mdb_event_m);
    k.mdb_event_holdoff = static_cast<long long>(std::max(0, nc.mdb_event_holdoff_ep));
    // Prior-residual census.  Nothing to resolve against the estimation configuration:
    // the census reads the (prior mean, prior var, posterior var) triples the carry
    // harvests already hold and writes counters, so the master arm is the predicate.  The
    // family CSV is parsed once, here, into per-family bools, because the feed sites sit
    // in the serial carry harvest and test a bool per row instead of searching a vector.
    // `prires_on` requires the arm and a non-empty set of known families, so a census
    // armed with only unknown family names (the loader already warns) enters no feed site.
    {
        k.prires_on = false;
        if (nc.prior_residual_census) {
            for (const std::string& f :
                 NetworkCarryVce::parseComponents(nc.prior_residual_families)) {
                if      (f == "pl")   k.prires_pl   = true;
                else if (f == "pb")   k.prires_pb   = true;
                else if (f == "cb")   k.prires_cb   = true;
                else if (f == "zwd")  k.prires_zwd  = true;
                else if (f == "iono") k.prires_iono = true;   // slant-iono carry (queue 8b)
                else if (f == "rxcb") k.prires_rxcb = true;
                else if (f == "clk")  k.prires_clk  = true;   // clock anchor (card Q3, INC-0)
            }
            k.prires_on = k.prires_pl || k.prires_pb || k.prires_cb
                       || k.prires_zwd || k.prires_iono || k.prires_rxcb || k.prires_clk;
        }
        // The corroboration join is gated on the master again here rather than relying on
        // the loader's error, so a hand-built NetworkConfig (tests) cannot arm a join
        // behind a census that never runs.
        k.prires_corrob       = k.prires_on && nc.prior_residual_corroborate;
        k.prires_report_sigma = (nc.prior_residual_report_sigma > 0.0)
                                ? nc.prior_residual_report_sigma : 4.0;
        k.prires_event_holdoff = std::max(0, nc.prior_residual_event_holdoff_ep);
        // Constraint-aware global σ̂₀, gated on the master as the second guard above.
        // `prires_s0g_redun` equals `mdb_from_raim`, for the same reason:
        // raim_redundancy_normalize alone computes no r_i, because networkPostFitRaim is
        // not called without network.raim.  It is resolved here rather than at the feed
        // site, so the r ≡ 1 fallback is decided once and the run-end line reports which
        // mode the whole run used.
        k.prires_s0g       = k.prires_on && nc.prior_residual_global_sigma0;
        k.prires_s0g_redun = config_.networkRaim()
                          && config_.networkRaimRedundancyNormalize();
    }
    // BlockFormal LOS block census.  The master is gated on a live host here rather than
    // relying on the loader's error, so a hand-built NetworkConfig (tests) cannot arm a
    // sweep behind a redundancy census that never runs.  The host predicate is the
    // census's own two-term expression, repeated as every other consumer repeats it;
    // there is no `network.redundancy_census` bool to read.
    {
        k.bb_on     = nc.block_baarda && k.redun_census_on;
        k.bb_corrob = k.bb_on && nc.block_baarda_corroborate;
        k.bb_report_sigma = (nc.block_baarda_report_sigma > 0.0)
                            ? nc.block_baarda_report_sigma : 4.0;
        k.bb_event_holdoff =
            static_cast<long long>(std::max(0, nc.block_baarda_event_holdoff_ep));
    }
    // Station congruence screen.  Same host predicate and the same second guard as the
    // block census, for the same reason: a hand-built NetworkConfig (tests) cannot
    // arm a sweep behind a redundancy census that never runs.  `cg_on` also gates the
    // assembly's geometry retention (INC-0), so resolving it here, once, before ctx.knobs
    // becomes const, keeps the producer and the assembly in agreement about whether
    // row_pos_enu exists.
    {
        k.cg_on = nc.congruence_screen && k.redun_census_on;
        k.cg_report_sigma = (nc.congruence_report_sigma > 0.0)
                            ? nc.congruence_report_sigma : 4.0;
        k.cg_event_holdoff =
            static_cast<long long>(std::max(0, nc.congruence_event_holdoff_ep));
    }
    // Prefit census, structural fuse, seed-birth tripwire, topology ledger and flight
    // recorder.  Nothing to resolve against the estimation configuration either: these
    // read the assembled row stacks and (the fuse) cap a weight, so the arming knobs are
    // the predicate, and the loader already clamps them.  `pc_walk_on` is the union of the
    // three increments that ride the walk: INC-3 and INC-4 need the classification pass
    // whether or not the INC-1 columns are requested.
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
    // Datum graph census.  Nothing to resolve against the estimation configuration: the
    // census reads the epoch's registration sets and the admitted support and writes
    // nothing back, so the master arm is the predicate.  It is not gated on
    // `sbasis_complete`: without SBC the spectral side still supplies the
    // observed-vs-declared count, the reconciliation drops to count level and the trace
    // records it (`obs_src=count`).  Disarming the census there would hide the tier-1
    // fragmentation reading, which needs no spectrum.
    k.dg_on            = nc.datum_graph_census;
    k.dg_event_holdoff = static_cast<long long>(std::max(0, nc.datum_graph_event_holdoff_ep));
    // Product-referenced measurement admission.  Nothing to resolve against the
    // estimation configuration either: the screen reads `observed` / `modeled` and removes
    // observables, so the arming knobs are the predicate, and the loader already clamps
    // the thresholds.  `adm_on` is the union of the response and the census because both
    // ride one pass; screening implies the census columns, so `adm_trace` is resolved as
    // the OR rather than copied.  The clamps are repeated here as a second guard, so a
    // hand-built NetworkConfig (tests) cannot arm a screen with a bound of zero and reject
    // the whole network.
    k.adm_screen   = nc.adm_screen;
    k.adm_trace    = nc.adm_trace || nc.adm_screen;
    k.adm_on       = k.adm_screen || k.adm_trace;
    k.adm_bound_m  = (nc.adm_bound_m > 0.0) ? nc.adm_bound_m : 1000.0;
    k.adm_min_sats = std::max(2, nc.adm_min_sats);
    k.adm_heal_ep  = std::max(1, nc.adm_heal_ep);
    k.adm_max_frac = (nc.adm_max_frac > 0.0 && nc.adm_max_frac < 1.0)
                     ? nc.adm_max_frac : 0.5;
    // Re-entry vet (INC-4).  The `&& adm_screen` repeats the loader's inert guard at the
    // predicate instead of relying on load order: a vet is an admission verdict and the
    // measure-only arm makes none, so a census-only run that reaches here with
    // `adm_revet` set does not rewrite arc records.  Same second-guard reason as the
    // clamps above.
    k.adm_revet        = nc.adm_revet && nc.adm_screen;
    k.adm_revet_gap_ep = std::max(1, nc.adm_revet_gap_ep);
    k.adm_revet_ep     = std::max(0, nc.adm_revet_ep);
    k.adm_revet_census = nc.adm_revet_census;
    k.adm_revet_hold   = nc.adm_revet_hold;
    // Resolved to metres here, once, so the seam compares against a number rather than
    // forming a product per channel per epoch.  The clamp is repeated here (a hand-built
    // NetworkConfig in a test could carry a scale of 0 and refuse every re-entry in the
    // network).  A scale above 1 would make the tighter vet bound looser than the
    // admission bound itself, inverting the knob, so an out-of-range scale falls back to
    // 0.2.
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
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
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
    const bool   pl_los_scale = ctx.knobs.pl_los_scale;   // per-LOS B_eff scale at inject (F1)
    const bool   pl_local_block = ctx.knobs.pl_local_block; // station-wide whitened local block at inject (F3)
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
        // Config fingerprint over the datum-relevant fields.  A resume under a changed
        // S-basis or pivot pin, station roster (its order defines the carry's station
        // indices), carry q values, gap bounds or floors, clock-model parameters, or
        // emitter channel config has to cold-start, because the carried state would be
        // read in the wrong frame or under the wrong rules.
        std::string fp;
        auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
        for (const auto& st : stations) { add(st.name); add(st.is_pivot ? "P" : "-"); }
        add(nc.clock_datum_station); add(nc.clock_datum_satellite);
        // With network.station_marker_to_arp armed, the roster's fixed coordinates are
        // reduced from the monument to the antenna reference point, so every carried
        // quantity that references a station's geometry (the phase-leveling zero-q levels,
        // the carried ambiguities, the local blocks) refers to a different position across
        // the flip (card UPO).  Appended only when armed, so the fingerprint of a snapshot
        // written with it off is unchanged.
        if (config_.networkStationMarkerToArp()) add("roster:arp");
        // Carrier-phase wind-up model revision.  Unlike the other tokens this is a code
        // constant, not a knob, because what changes the meaning of the carried state is a
        // source fix, not a config.  The earlier dipole sign left +2*phi_wu in every phase
        // observation (about 0.4 cycles of ZD arc range), which the carried ambiguities,
        // the phase-leveling zero-q level and the shared PB/CB carry absorbed.  Resuming a
        // snapshot written under the old convention into a corrected binary would
        // re-reference stiff carries levelled in the superseded frame (standing
        // invariant #1, a stale zero-q value carried past a jump, and invariant #7, a
        // re-coupling edge with nothing re-validating what it reconnects).  The rest of
        // the fingerprint hashes config knobs only, with no code version or binary hash,
        // so this token is what forces a cold start across the fix.  See
        // docs/SPEC_phase_windup_sign.md.
        add(kWindUpYawModelId);
        // The phase-bias gauge realization (single-pivot vs prioritized anchor)
        // defines which station's ambiguities carry the datum; a resume across a
        // toggle change would interpret the carried anchors in the wrong frame.
        add(fmt::format("{}", phase_datum_prioritized));
        // The anchor-free ensemble datum (fix A) references the receiver ensemble
        // (Σ_r z=0), a different carried-ambiguity frame than the single-anchor pivot or
        // prioritized datum (both of which set phase_datum_prioritized).  Appended only
        // when armed, so a non-ensemble snapshot's fingerprint is unchanged.
        if (phase_datum_ensemble) add("phdatum:ens");
        // The eq34 chain datum references a different carried-ambiguity frame
        // (per-receiver RPB split plus spanning-tree pins) than the anchor, pivot or
        // ensemble datums, so a resume across the flip has to cold-start.  Appended only
        // when armed, so a non-eq34 snapshot's fingerprint is unchanged.
        if (phase_datum_eq34) add("phdatum:eq34");
        // The invariant carry snapshots a DD z-store that the legacy carries do not have,
        // so a resume across a carry_invariant flip has to cold-start (card 4, OPEN-4).
        // Appended only when armed, so the fingerprint of a snapshot without it is
        // unchanged.
        if (carry_invariant) add("carry:inv");
        // PD-safe DD fold.  The carried Pᶻ a pd_safe run writes comes from a different
        // recursion (product-form posterior, equilibrated factorization, eigen-clamp
        // repairs, degenerate blocks rebirthed rather than frozen) and is PD by
        // construction.  Resuming one arm's z-store into the other would mix the two
        // arithmetics inside a single lineage, which is the mid-chain flip the fingerprint
        // exists to prevent.  Appended only when armed, so the fingerprint of a snapshot
        // written with it off is unchanged.  (A snapshot from an off run restored into a
        // pd_safe run is handled separately: the INC-2c restore guard rebirths any non-PD
        // block it finds in the blob.)
        if (carry_invariant_pd_safe) add("carry:pdsafe");
        // network.carry_aware_covariance is left out of the fingerprint on purpose.  It is
        // a read-back knob: its two consumers (dd_sigma_cyc, the log-only AR probe) feed
        // nothing back, so no byte of the snapshot (carry blob, stash, emitter state)
        // changes meaning when it flips.  Fingerprinting it would strand every existing
        // snapshot for a trace column; it follows the loader's fingerprint-safe list of
        // trace-class knobs that stay armable on a warm resume against an untraced
        // snapshot (GnssConfigLoadNetwork.cpp).  A later knob that routes the correction
        // into the phase-leveling stash (the stored prior variance) is a different knob
        // and has to be fingerprinted.
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        // Per-LOS scale (F1, network.phase_leveling_los_scale).  The stash is unchanged,
        // but the inject-side weighting of the same stashed variances differs across the
        // flip (the carried level was accumulated under a different recursion), so a
        // resume across it has to cold-start, as for pl_var_floor.  Appended only when
        // armed, so the fingerprint of a snapshot written with it off is unchanged.
        if (pl_los_scale) add("pl:losscale");
        // Local block (F3, network.phase_leveling_local_block): the same rule for a
        // stronger reason.  The carried levels were accumulated under a different prior
        // (joint against independent rows), and the harvest block itself is not in the
        // snapshot.  Appended only when armed, beside the F1 tag.
        if (pl_local_block) add("pl:losblock");
        // ZWD in the local block (B9-in-block, network.carry_zwd_in_block) affects the
        // solve for the same reason F3 does: the carried ZWD and the carried levels were
        // accumulated under a different prior geometry (one joint Σ over {ambiguities,
        // ZWD} against a block plus an independent diagonal row), so a warm resume cannot
        // cross the flip.  The harvest block is not snapshotted either way.  Appended only
        // when armed, beside the F1/F3 tags, so a snapshot written with it off keeps its
        // fingerprint.
        if (config_.networkCarryZwdInBlock()) add("zwd:blk");
        // Iono in the local block (network.carry_iono_in_block) affects the solve for the
        // same reason: the carried levels were accumulated against a prior geometry that
        // did or did not include the station's iono columns in the joint Σ, so a warm
        // resume cannot cross the flip.  The in-block iono carry itself is not snapshotted
        // (it cold-starts), but the levels that resume were shaped by it.  Appended only
        // when armed, beside the F1/F3/ZWD tags, so a snapshot written with it off keeps
        // its fingerprint.  q and the gap bound are included because both change the
        // prior's weight.
        if (config_.networkCarryIonoInBlock()) {
            add("iono:blk");
            add(fmt::format("{}", config_.networkCarryIonoInBlockQM2()));
            add(fmt::format("{}", config_.networkCarryIonoInBlockMaxGapS()));
        }
        // Receiver clock in the local block (network.carry_rx_clock_in_block) affects the
        // solve for the same reason: the carried levels were accumulated against a prior
        // geometry that did or did not include the station's receiver-clock column in the
        // joint Σ, so a warm resume cannot cross the flip.  The clock carry itself is not
        // snapshotted (it cold-starts, which gives one epoch of white-clock behaviour), but
        // the levels that resume were shaped by it.  Appended only when armed, beside the
        // F1/F3/ZWD/iono tags, so a snapshot written with it off keeps its fingerprint.
        // The whole variance ladder is included: q, rate_q, rate_var0 and the ISB flag
        // each change the prior's weight, and jump_k changes which epochs carried anything.
        if (config_.networkCarryRxClockInBlock()) {
            add("rxclk:blk");
            add(fmt::format("{}", config_.networkCarryRxClockQM2()));
            add(fmt::format("{}", config_.networkCarryRxClockMaxGapS()));
            add(fmt::format("{}", config_.networkCarryRxClockRateVar0()));
            add(fmt::format("{}", config_.networkCarryRxClockRateQ()));
            add(fmt::format("{}", config_.networkCarryRxClockJumpK()));
            add(fmt::format("{}", config_.networkCarryRxClockIsb()));
        }
        // Second code channel (card SCC, R6): adds a receiver inter-signal state per
        // station and band-1 companion, carried by the receiver code-bias carry whose
        // stash is in the snapshot.  A resume across the flip would restore (or fail to
        // restore) receiver code-bias columns that the other side does not have, and the
        // companion code rows that determine them change the local block every epoch, so
        // the knob affects the solve on both counts.  Appended only when armed, beside the
        // F1/F3/ZWD/iono/rxclk tags, so a snapshot written with it off keeps its
        // fingerprint.  The allow-list and the sigma scale are included: the list decides
        // which columns exist, and the scale sets the weight at which their rows were
        // accumulated.
        // All-channel mode (card SID phase 6b, R8) decides which columns exist: a receiver
        // inter-signal code state per extra code and per band, a float ambiguity per extra
        // phase, and the shared satellite families those rows feed.  A resume across the
        // flip would restore a carry whose column set the other side does not have, the
        // same argument as for SCC and SID phase 6a.  Appended only when armed, so a
        // snapshot written with it off keeps its fingerprint.
        if (config_.networkChannelModeAll()) add("chanmode:all");
        if (config_.networkSecondCodeChannel()) {
            add("scc:2ndcode");
            for (const auto& sig : config_.networkSecondCodeSignals()) add(sig);
            add(fmt::format("{}", config_.networkSecondCodeSigmaScale()));
        }
        // Second phase channel (card SID phase 6a, R6): adds a float ambiguity per extra
        // arc and feeds an existing satellite phase-bias family from stations that did not
        // support it before, so the carried levels and the shared border in a snapshot
        // mean different things across the flip.  Appended only when armed, as for SCC,
        // so a snapshot written with it off keeps its fingerprint.  The allow-list and the
        // sigma scale are included: the list decides which arcs exist, and the scale sets
        // the weight at which their rows were accumulated.
        if (config_.networkSecondPhaseChannel()) {
            add("sid:2ndphase");
            for (const auto& sig : config_.networkSecondPhaseSignals()) add(sig);
            add(fmt::format("{}", config_.networkSecondPhaseSigmaScale()));
        }
        // Ledger re-entry response (card SID phase 3, R6): the gap re-entry response keys
        // on the arc ledger's admitted lifecycle, which decides which carried levels
        // survive a break.  A resume across the flip would restore levels that the other
        // side would have refloated, and the ledger is not snapshotted (by design), so the
        // two sides cannot be reconciled afterwards.  Appended only when armed, as for
        // SCC, so a snapshot written with it off keeps its fingerprint.  The no-verdict
        // policy is included because it decides the disposition of the arcs the raw
        // tiers never saw.
        if (config_.networkLedgerReentry()) {
            add("sid:ledger_reentry");
            add(config_.networkLedgerReentryNoVerdict()
                    == LedgerReentryNoVerdict::Refloat ? "refloat" : "bridge");
        }
        // Election memory across a gap (card SID phase 4b, B3).  When armed, a family that
        // the coverage gate erased and that returns at the same generation is re-pinned on
        // its previous incumbent rather than on the station the coverage rank picks, so
        // the carried ambiguities a resumed run restores were accumulated against a
        // different realization of that family's datum.  The drop memo that decides it is
        // not snapshotted (by design), so the two sides cannot be reconciled afterwards.
        // Appended only when armed, as for SCC, so a snapshot written with it off keeps
        // its fingerprint.  network.ledger_screens is not appended: it is an observatory
        // and changes no byte of the solve, the same trace-knob rule the loader's
        // fingerprint-safe list applies.
        if (config_.networkLedgerAnchor()) add("sid:ledger_anchor");
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
        // Handover S-transform (queue 1(b)): it changes which carried priors fold at a
        // pivot switch, so a resume across a toggle change would read a carry that was
        // (or was not) re-expressed in the wrong datum.  Appended only when armed, as for
        // phase_datum_ensemble and phase_leveling_los_scale, so the off path keeps its
        // fingerprint.  When armed, the lineage needs a fresh convergence.
        if (ctx.knobs.handover_stransform) add("sts:1");
        // Card PRM phase 2: the transform before the coupling rows changes the handover
        // epoch's rows (armed-only token, so every existing fingerprint is unchanged).
        if (ctx.knobs.handover_stransform_before_rows) add("stsrows:1");
        // Card SCL B2: the exact S P S^T and the anchor kept across the handover change the
        // handover epoch's fold and the anchor's state (armed-only tokens, so every existing
        // fingerprint is unchanged).
        if (ctx.knobs.handover_stransform_dense) add("stsdense:1");
        if (ctx.knobs.cta_datum_keep) add("ctakeep:1");
        // Card PRM phase 3b step 2: the re-base mechanisms also move their owed sites, which
        // changes the carry they hand on (armed-only token, so every existing fingerprint is
        // unchanged).
        if (ctx.knobs.rebase_complete) add("rbc:1");
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        // External satellite code bias (EXT-CB): the satellite code bias moves from a
        // shared column to a modelled correction on the observable, and the forced rx DCB
        // adds a local state, so a carried shared border means something different with it
        // armed and a warm resume cannot cross the flip.  The (sat_code_bias, iono_sh) pair
        // above reads (false, true) only in this posture, but that is an inference from a
        // coupling, so the knob itself is appended to state it.  Appended only when armed
        // (the rxcb/shl2/iono2 pattern), so a snapshot written with it off keeps its
        // fingerprint.
        if (config_.networkExternalSatCodeBias()) add("extcb:1");
        // The receiver code-bias datum (RXCB-DATUM) changes which receiver-DCB combination
        // is realized, so the carried rx-CB levels mean something different with it armed.
        // Appended only when armed, like the others.
        if (config_.networkRxCodeBiasDatum())
            add(fmt::format("rxcbdatum:{}", config_.networkRxCodeBiasDatumSigmaM()));
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // Slant-iono carry (queue 8b) affects the solve: the leveling levels a chain
        // harvests under iono-carried solves mean something different than under white
        // iono, so a warm resume cannot cross the flip even though the carry map itself is
        // not snapshotted (it cold-starts).  Appended only when armed (the rxcb/shl2/iono2
        // pattern), so a snapshot written with it off keeps its fingerprint.
        // Card LGM: the phase nuisance state changes what every carried ambiguity level
        // means (the colour is no longer in it), and its own carry is not snapshotted, so a
        // warm resume cannot cross the flip.  Appended only when armed.
        if (nc.phase_colour_state)
            add(fmt::format("pcol:{}:{}:{}:{}", nc.phase_colour_sigma_zen_m, nc.phase_colour_tau_s,
                            nc.phase_colour_elev_floor_deg, nc.phase_colour_max_gap_s));
        if (config_.networkCarryIono())
            add(fmt::format("ionocarry:{}:{}:{}:{}:{}", config_.networkIonoCarryScope(),
                            config_.networkIonoCarrySource(),
                            config_.networkIonoCarryQ(), config_.networkIonoCarryVarFloor(),
                            config_.networkIonoCarryMaxGapS()));
        // rx-CB carry: appended only when armed (the shl2/iono2 pattern), so a snapshot
        // written with it off keeps its fingerprint.
        if (rx_cb_on) {
            add("rxcb:1"); add(fmt::format("{}", rx_cb_q));
            // The de-mean (card LV-2) changes what a carried rx-CB level means: the carry
            // stops transporting the common mode, so a chain that folded demeaned priors
            // and one that did not have realized different receiver-DCB frames.  Appended
            // only when armed, nested inside rxcb:1 (itself conditional).
            if (ctx.knobs.rx_cb_demean) {
                add(fmt::format("rxcbdemean:{}", ctx.knobs.rx_cb_demean_min_sta));
                // Within-epoch de-mean (card LV-5): a chain that folded against this
                // epoch's μ and one that folded against last epoch's have realized
                // different receiver-DCB frames.
                if (ctx.knobs.rx_cb_demean_within) add("rxcbdemeanwin:1");
            }
            // Step gate (card LV-4): it changes which priors fold and which carried entries
            // survive an event, so a resume cannot cross the flip.  Nested and appended
            // only when armed, like the others.
            if (ctx.knobs.rx_cb_step_gate)
                add(fmt::format("rxcbstep:{}", ctx.knobs.rx_cb_step_k));
            // The S-consistent harvest re-zeros the carried set's common mode every epoch,
            // so a stored snapshot's absolute rx-CB levels are in a different frame than
            // those of an untransformed chain, and a resume cannot cross the flip.  Nested
            // and appended only when armed, like the others.
            if (ctx.knobs.rx_cb_harvest_demean)
                add(fmt::format("rxcbharv:{}", ctx.knobs.rx_cb_harvest_screen_z));
        }
        // The clock anchor changes the temporal model of the carried satellite clock: a
        // snapshot written by an anchored run carries variances produced by a different
        // process, and its per-satellite anchored/loose state is not carried across on
        // purpose (a warm resume is an invariant #1 event, so every satellite re-earns the
        // anchor).  Mixing the two inside one lineage would fold a stiffness without the
        // evidence behind it.  Appended only when armed, so a snapshot written with it off
        // keeps its fingerprint.
        if (ctx.knobs.cta_on)
            add(fmt::format("cta:{}:{}:{}:{}:{}:{}:{}:{}", ctx.knobs.cta_q,
                            ctx.knobs.cta_var_floor, ctx.knobs.cta_max_gap_s,
                            ctx.knobs.cta_k, ctx.knobs.cta_abs_m,
                            ctx.knobs.cta_sigma_floor_m, ctx.knobs.cta_recouple_ep,
                            ctx.knobs.cta_resolve));
        // Gauge-free fold (§2.1): a snapshot written by a gauge-free run holds a carry whose
        // clock information is in a different subspace, so a resume cannot cross the
        // flip.  Appended only when armed, so a plain-anchor lineage keeps its fingerprint.
        // The token follows the resolved predicate (card C-GF), so arming the downdate
        // through the carry knob invalidates a snapshot in the same way as arming it through
        // the Q3 knob.  For a config with shared_carry_gauge_free off the token is
        // unchanged, since carry_gauge_free == cta_on && cta_gauge_free there.  The closure
        // test is part of the token, not a separate one: it changes which directions are
        // marginalized out of the carried block, so crossing a closure flip is the same
        // incompatibility as crossing the gauge-free flip.  The legacy arming (no closure)
        // keeps the bare "cta:gf" token, so existing snapshots still match.
        if (ctx.knobs.carry_gauge_free)
            add(ctx.knobs.carry_gauge_free_closure ? "cta:gf+cl" : "cta:gf");
        // Guard statistic (§2.1 bisection): the denominator decides which epochs trip, and a
        // trip withdraws a prior and re-solves the epoch, so two runs that differ only here
        // produce different carried state and cannot resume across each other.  Appended
        // only when the resolved pair differs from what `auto` would choose, so a lineage
        // on `auto` keeps its fingerprint.
        if (ctx.knobs.cta_on
            && (ctx.knobs.cta_stat_proj_post != ctx.knobs.cta_gauge_free
                || ctx.knobs.cta_stat_proj_prior))
            add(fmt::format("cta:gfstat:{}:{}", ctx.knobs.cta_stat_proj_post ? 1 : 0,
                            ctx.knobs.cta_stat_proj_prior ? 1 : 0));
        // Card CTU section 14: the phase-space guard decides trips, which changes the carried
        // state; the shadow mode is trace-only.  Appended only in phase mode.
        if (ctx.knobs.cta_on && ctx.knobs.cta_space == 2)
            add(fmt::format("cta:space:phase:{:.4f}", ctx.knobs.cta_phi_floor_m));
        // The clock level re-base (card CLV) moves the carried clock and phase-bias values
        // (and every coupling term that references them), so a snapshot written with it
        // armed holds a different state than one written without it.  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (ctx.knobs.clv_on)
            add(fmt::format("clv:{}:{}", ctx.knobs.clv_floor_m, ctx.knobs.clv_horizon_s));
        // Card CLV return plan: appended only when a sub-knob leaves its default, so a
        // snapshot written by the built card keeps its fingerprint.
        if (ctx.knobs.clv_on
            && (ctx.knobs.clv_pin_pair_read || ctx.knobs.clv_cb_follow != 0
                || ctx.knobs.clv_datum_pause || ctx.knobs.clv_confirm_epochs != 1
                || ctx.knobs.clv_pivot_ref))
            add(fmt::format("clvrp:{}:{}:{}:{}:{}", ctx.knobs.clv_pin_pair_read ? 1 : 0,
                            ctx.knobs.clv_cb_follow, ctx.knobs.clv_datum_pause ? 1 : 0,
                            ctx.knobs.clv_confirm_epochs, ctx.knobs.clv_pivot_ref ? 1 : 0));
        // Card CLV2: the batch-2 knobs move further carried values (the free code-bias
        // columns), so a snapshot written with one armed carries a different state.
        // Conditional append, so every earlier fingerprint is unchanged.
        if (ctx.knobs.clv_on && ctx.knobs.clv_free_cb_rebase)
            add("clv2:fcb");
        // Card HLT: the health rule changes the clock pivot ("pivot") or the satellite set
        // ("exclude"), so a snapshot written with it armed carries a different datum or
        // state.  Conditional append, so every earlier fingerprint is unchanged.
        if (ctx.knobs.sat_health != 0)
            add(fmt::format("hlt:{}", ctx.knobs.sat_health));
        // The seed reframe (card LV-1) changes the per-station linearization point of the
        // per-LOS iono and, through initializeCodeBiases (which subtracts the seeded iono),
        // of the receiver DCB.  The absolute solve is invariant to that frame, but the
        // snapshot stores absolute carried levels whose prefit innovations are read against
        // x_lin (the rx-CB inject gate is such a consumer), so a resume cannot cross the
        // flip.  Appended only when armed.
        if (ctx.knobs.lv1_seed_frame)
            add(fmt::format("lv1seed:{}", ctx.knobs.lv1_min_sats));
        // Carry VCE apply (M2 v2): the variance factor multiplies every carry fold
        // variance, so a carried (value, var) pair means a different prior with it armed
        // than without.  That makes it a fingerprinted knob, unlike the carry_vce monitor,
        // which is fingerprint-free.  The fingerprint is necessary but not sufficient here:
        // both legs of an apply-on chain fingerprint identically while α² itself is not
        // snapshotted, so networkTryWarmResume refuses the resume outright (spec §8, the
        // one blocking deferral).  Appended only when armed, so a snapshot written with it
        // off keeps its fingerprint.
        if (config_.networkCarryVceApply()) {
            add("vceapply:1");
            add(config_.networkCarryVceApplyFamilies());
            add(fmt::format("{}", config_.networkCarryVceApplySlew()));
            add(fmt::format("{}", config_.networkCarryVceApplyMax()));
            add(fmt::format("{}", config_.networkCarryVceApplyMinRows()));
            add(fmt::format("{}", config_.networkCarryVceComponents()));
            add(fmt::format("{}", config_.networkCarryVceWindowEpochs()));
        }
        // apriori_source and the anchor σ are datum-relevant.  The carried clock frame is
        // Δδtˢ against broadcast under brdc_rts and Δδtˢ against precise under sp3 (they
        // differ by metres), and only brdc_rts carries the relevel_iode baseline, so a warm
        // resume across an apriori_source change would fold a clock carry in the wrong
        // frame and re-level against a stale baseline.
        add(fmt::format("{}", static_cast<int>(config_.networkAprioriSource())));
        add(fmt::format("{}", config_.networkAprioriClockAnchorSigmaM()));
        // Card NBP: the selection rule defines which ephemeris the carried clock refers to
        // through the offset table, so it is part of the frame.  Appended only under
        // sp3_brdc_clock (whose enum value already differs), so every other fingerprint
        // is unchanged.
        if (config_.networkAprioriBrdcClock())
            add(fmt::format("nbp:sel={}", static_cast<int>(config_.networkBrdcSelect())));
        // The clock-gauge realization defines the frame of the carried clock states (pin,
        // sum-to-zero and broadcast-anchored differ by per-constellation or common-mode
        // constants), so a resume across a realization change has to cold-start.  Appended
        // only when a non-default realization is armed, so a pin-mode snapshot keeps its
        // fingerprint.
        if (clock_sum_to_zero || clock_brdc_anchored)
            add(fmt::format("clkdatum:s2z={},bca={}", clock_sum_to_zero,
                            clock_brdc_anchored));
        // The optional emitter discontinuity channels are construction parameters of the
        // snapshotted emitter state (restoreState fails on a mismatch); appended only when
        // armed, so default fingerprints are unchanged.
        if (config_.networkCodeBiasDiscJumpTest() || config_.networkClockDiscOnSetChange())
            add(fmt::format("emitchan:cbj={},cks={}", config_.networkCodeBiasDiscJumpTest(),
                            config_.networkClockDiscOnSetChange()));
        // The de-commoned phase jump test (card JT) decides which (sat,signal) counters
        // step, and those counters are snapshotted emitter state, so a resume across a
        // change of this knob would continue a counter series produced under the other
        // test.  It adds no serialized state of its own (the de-commoned step is a pure
        // function of the epoch and last_phase_bias_), so it is fingerprinted here rather
        // than in the emitter header.  Appended only when armed, so a snapshot written
        // with it off keeps its fingerprint.
        if (config_.getNetworkConfig().disc_jump_test_decommon)
            add("discdecom=1");
        // The same argument applies to the per-channel mark scope (card MS): it decides
        // which channel's counters a landed mark steps, those counters are snapshotted
        // emitter state, and the pending-mark section carries a channel mask.  Appended
        // only when armed, so a snapshot written with it off keeps its fingerprint.
        if (config_.getNetworkConfig().disc_mark_scope == "channel")
            add("discscope=channel");
        // The held clock signal basis defines which IF combination each carried clock
        // state is referenced to; a resume across a toggle change would mix clock states in
        // a frame re-selected per epoch with states in a held frame.  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (clock_signal_basis_hold)
            add(fmt::format("clkbasishold={}", clock_signal_basis_hold));
        // Excluding unmatched-antenna stations from phase-bias emission changes which
        // per-(sat,signal) phase-bias states the carried solution holds; a resume across a
        // toggle change would carry a different shared-state set.  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (pb_excl_unmatched)
            add(fmt::format("pbexclunmatched={}", pb_excl_unmatched));
        // Epoch decimation changes the carried-state cadence, so a resume has to match it
        // (T2e).  Appended only when != 1, so a config without decimation keeps its
        // fingerprint.  The DD-trace stride is trace-only and left out on purpose: arming
        // a trace on resume does not invalidate an untraced snapshot.
        if (nc.epoch_decimation_n != 1)
            add(fmt::format("edecim={}", nc.epoch_decimation_n));
        // The GM deviation (T5) changes the estimation (it is not trace-only), and its
        // carry is not persisted in the snapshot, so a resume across a tau change has to
        // cold-start.
        if (shc.rep_error_tau_s > 0.0)
            add(fmt::format("shdevtau={}", shc.rep_error_tau_s));
        // Dual-layer ionosphere (Iono-2): the second shell adds carried SH coefficients
        // whose meaning depends on its degree and height (a resume across a re-layering
        // would fold layer-fitted coefficients into a different shell geometry).  Appended
        // only when armed, so a single-layer config keeps its fingerprint.
        if (shc.layer2_max_degree > 0)
            add(fmt::format("shl2={}@{}", shc.layer2_max_degree, shc.layer2_height_km));
        // The conditional local carry changes what the per-station carry means (a
        // shared-coupled conditional row against an independent diagonal marginal), so a
        // resume across a toggle change would fold a carry of the wrong kind; that is the
        // reason for the token.  The per-column coupling data (M_r rows, ŝ_prev, P_prev) is
        // persisted, but only into a format-v3 snapshot written under
        // network.state_snapshot_coupling_capture, and only a reader that arms
        // network.resume_coupling_restore rebuilds it (§18.12); a v1/v2 file carries none
        // of it, and a restored row degrades to a plain diagonal.  The token stays in
        // either case (removing it would strand every stored seed).  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (carry_cpl_cond)
            add(fmt::format("carrycplcond={}", carry_cpl_cond));
        // The time-linked code-bias datum changes what the emitted OSB/clock split means (a
        // time-continuity datum against a per-epoch set-mean datum), and its
        // per-(const,signal) previous-value map is persisted only under
        // network.state_snapshot_datum_capture, so a resume across a toggle change would
        // compare a solve in one datum against a carry in the other.  Appended only when
        // armed, so a snapshot written with it off keeps its fingerprint.
        if (cb_time_linked)
            add(fmt::format("cbtimelink={}", cb_time_linked));
        // The border time update changes what an injected coupling row means (a κ-shrunk,
        // re-independenced pseudo-observation of the moved border against the raw
        // harvest-epoch row), and the κ inputs (per-column P_prev and the q ladder) are not
        // persisted in the v1 snapshot.  Appended only when armed, so a snapshot written
        // with it off keeps its fingerprint.
        if (carry_cpl_tu)
            add(fmt::format("carrycpltu={}", carry_cpl_tu));
        // The exact update (card CTU) changes every injected row's coefficients and variance
        // again, on the same footing; appended only when armed.
        if (ctx.knobs.carry_cpl_exact)
            add(fmt::format("carrycplexact={}", ctx.knobs.carry_cpl_exact));
        // Card WSX: the within-station bracket changes the rows' joint weight; armed only.
        if (ctx.knobs.carry_cpl_exact_cross)
            add(fmt::format("carrycplexactx={}", ctx.knobs.carry_cpl_exact_cross));
        // Card PFQ: the pivot-frame clock noise changes every carried clock's prior and the
        // exact update's Q_S; armed-only token.
        if (ctx.knobs.pfq_on)
            add(fmt::format("pfq={}", ctx.knobs.pfq_on));
        // Card SCL: the clock clone changes the rows, the fold and the carry each epoch hands
        // on (the harvest marginalizes the clones); armed-only token, so every existing
        // fingerprint is unchanged.
        if (ctx.knobs.carry_cpl_clone) add("ccl:1");
        // Card SCL2 X2: the shared history changes the rows, the fold and the carried
        // variances (the remainder only); armed-only token with its rank.
        if (ctx.knobs.carry_cpl_shx) add(fmt::format("shx:{}", ctx.knobs.shx_rank));
        // Card BCL: the bias and departed clones change the rows and the fold; armed-only
        // tokens.
        if (ctx.knobs.carry_cpl_bclone) add("bcl:a");
        if (ctx.knobs.carry_cpl_dclone) add("bcl:d");
        // The bound raise (card CTU) moves the carried border block; the monitor alone is
        // trace class and stays out of the fingerprint.
        if (ctx.knobs.clm_raise)
            add(fmt::format("clmraise={}", ctx.knobs.clm_raise));
        // RXCB-IN-BLOCK changes what a carried receiver code-bias entry is (a block row with
        // a border regression instead of a diagonal prior); appended only when armed.
        if (ctx.knobs.rxcb_blk)
            add(fmt::format("rxcbblk={}", ctx.knobs.rxcb_blk));
        // A drop that decouples at birth strength changes the carried state; appended
        // only when armed (card CTU section 14).
        if (ctx.knobs.amb_drop_decouple_always)
            add("ambdropdecouple=true");
        // Marginalized against carried SH coupling (card 1d-SH) changes what an armed
        // carry means; appended only when armed, so existing fingerprints are unchanged.
        if (carry_cpl_shmarg)
            add(fmt::format("cplshmarg={}", carry_cpl_shmarg));
        // Gauge projection and source re-init (cards 1f and 1g) both change which
        // information an injected coupling row carries: 1f alters the coefficient vector
        // of every truncated row, and 1g moves whole columns onto the departed route.  Same
        // footing as carry_cpl_tu above; appended only when armed, so existing
        // fingerprints are unchanged.
        if (ctx.knobs.carry_cpl_gauge)
            add(fmt::format("carrycplgauge={}", ctx.knobs.carry_cpl_gauge));
        if (ctx.knobs.carry_cpl_reinit)
            add(fmt::format("carrycplreinit={}", ctx.knobs.carry_cpl_reinit));
        // Joint whitening reshapes the injected coupling rows (departed columns go to a
        // shared M_w marginalization instead of a per-row scalar σ²), which determines the
        // solve in the same way as carry_cpl_cond/_tu/_shmarg above; all of them gate a
        // resume.  Without this token, a warm resume that flips joint whitening with
        // carry_cpl_cond still on would match fingerprints and diverge.  Appended only when
        // armed, so a non-joint fingerprint is unchanged; joint requires carry_cpl_cond, so
        // an armed-joint config also carries carrycplcond.
        if (carry_cpl_joint)
            add(fmt::format("carrycpljoint={}", carry_cpl_joint));
        // The CB-excursion guard (SPEC_day129_frame_guards phase 2.1) changes what a
        // carried code bias means across an excursion (a reinit-decorrelated state against
        // a stiff continuation), and its pending-bump queue is not snapshotted, so a resume
        // that flips it would fold a carry of the wrong kind and could drop a queued
        // discontinuity without a trace.  Same footing as cbtimelink / carrycpl* above.
        // Appended only when armed, so an unarmed fingerprint is unchanged.
        if (nc.cb_excursion_reinit)
            add(fmt::format("cbexc={},{}", nc.cb_excursion_median_m,
                            nc.cb_excursion_reinit_all_sats));
        // --- Not fingerprinted: the forward-only policy exemption ---
        //
        // The knobs listed below are left out of the fingerprint for one shared reason,
        // recorded here so that the omission is not "fixed" later.
        //
        // A knob is exempt when it is a forward-only policy: it changes what the run does
        // from here on (which rows fold, at what weight, which observables are admitted,
        // which arcs re-tap, what a resume rebuilds), but it re-references no carried
        // quantity retroactively and changes the meaning of no serialized state.  A
        // carried ambiguity, code bias, slant or ZWD is the same quantity in the same frame
        // whether or not a screen decides what feeds it next epoch.  Each such knob's own
        // state is per-epoch or per-resume and is never serialized, so a resumed leg starts
        // with it cold.  That is the conservative direction, because a resume is a window
        // in which no time-difference detector can operate.
        //
        // In practice, a fingerprinted knob cannot be A/B tested on one snapshot; keeping
        // these out lets every off/on validation pair replay from a shared seed under
        // `state_snapshot_require: true`.
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
        //                           resume_signal_canon_restore (RS-8, §21.16),
        //                           resume_border_restore (RS-7, §21.14),
        //                           resume_rxcb_lin_restore (RS-9, §21.18),
        //                           zwd_seam_reseed_q (RS-10b, §21.21)
        //
        // Notes on individual entries:
        //
        //   adm_revet reads a cross-epoch record, which looks fingerprint-relevant but is
        //   not: `vet_left` / `last_ep` live in the same unserialized record map as
        //   `admitted`, so a resumed leg opens no vet windows and every channel reads as a
        //   first sighting, which is the restore case and is already handled.
        //   (`adm_resume_pending` makes the first restored epoch a screened restore rather
        //   than a birth, the one case the seed-birth tripwire cannot see.)
        //
        //   state_snapshot_datum_capture (RS-3) is a representation question like the
        //   coupling capture below, with one difference: its rows use the existing header
        //   (as the optional var_first field does), so a binary that predates it rejects
        //   the file on the unknown record tag rather than on the header literal.  Both are
        //   this format's documented cold-start signal and both throw under
        //   state_snapshot_require, so the safety is the same; keeping the header stable
        //   lets an RS-3 A/B diff two snapshots and see only the new rows.  The datum
        //   quantities themselves are fingerprinted by the knobs that create them
        //   (phase_datum_eq34, code_datum_time_linked), so a writer/reader mismatch on
        //   their meaning cannot occur.
        //
        //   state_snapshot_coupling_capture is a serialization representation, not a
        //   policy: it adds rows and bumps the header to v3.  That is a question of binary
        //   vintage (a reader that predates it fails the header check and cold-starts), not
        //   of the fingerprint, and the writer's products and traces stay byte-identical
        //   because the capture block reads ctx.carry after the emit and writes only into
        //   `snap`.  The card-1 arithmetic tokens those rows depend on are already in fp
        //   above, so a writer/reader mismatch on the coupling arithmetic cannot occur.
        //   The same reasoning covers the unconditional Z-row and frame-row captures and
        //   the RS-5 emit-layer (state_snapshot_emit_capture) and RS-6 station-layer
        //   (state_snapshot_station_capture) captures: both add rows under the existing
        //   header and leave the writer's products and traces byte-identical, and the
        //   quantities themselves are fingerprinted by the knobs that create them
        //   (carry_invariant_emit_rebase / emit_pb_filter for the emit layer;
        //   carry_rx_code_bias and global_vtec_sh.rep_error_tau_s for the two station
        //   carries), so a writer/reader mismatch on their meaning cannot occur.
        //
        //   state_snapshot_signal_capture (RS-8, §21.16) is the same representation
        //   question: CN rows under the existing header, read from the station models after
        //   the emit, with the writer's products and traces byte-identical.  The station
        //   roster (which defines what the row's index means) is already in fp above, so an
        //   index cannot name a different station undetected.  Its own contents
        //   (signal_priorities and the coverage allowlist) are left out on purpose: they are
        //   not in the fingerprint, adding them would strand every stored snapshot, and
        //   a mismatch is visible anyway.  A restored channel that this run does not track
        //   has no observation, which is what the band reads without the restore (a station
        //   that tracks nothing on a band has no canonical for it either way), and the
        //   restore audit reports the disagreement.
        //
        // None of these belongs in the fingerprint: adding one would strand every stored
        // snapshot and rule out the same-snapshot off/on A/B that its validation depends
        // on.  (zwd_on / zwd_q above stay fingerprinted, because the continuous carry
        // affects the solve on both sides of a seam.)
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
        // Card CTU G5: the same fingerprint with the card's own tokens removed, so a run
        // that switches the exact update, the bound raise, the in-block rx-CB carry or the
        // phase-space guard ON can resume from a snapshot written without them (the
        // switch-on seam the card's G5 grades).  The reverse direction is not accepted:
        // a snapshot written with them holds the card's state, which a run without them
        // would read under the old rules.  Only under network.ctu_switch_on_resume: the
        // seam failed G5, and card EXL's ledger rows (the knobs armed on the goldens) are
        // fresh convergence only.  Off, the twin is 0 and the resume gate compares the
        // live fingerprint alone.
        ctx.carry.live_fingerprint_ctu_off = 0;
        if (config_.networkCtuSwitchOnResume()) {
            std::string off = fp;
            for (const std::string& tok : {std::string("carrycplexact=true"),
                                           std::string("clmraise=true"),
                                           std::string("rxcbblk=true"),
                                           std::string("ambdropdecouple=true")}) {
                const std::string t = tok + '\x1f';
                const auto pos = off.find(t);
                if (pos != std::string::npos) off.erase(pos, t.size());
            }
            const auto cs = off.find("cta:space:phase:");
            if (cs != std::string::npos) {
                const auto end = off.find('\x1f', cs);
                if (end != std::string::npos) off.erase(cs, end - cs + 1);
            }
            ctx.carry.live_fingerprint_ctu_off =
                (off != fp) ? NetworkStateSnapshotIO::fnv1a64(off) : 0;
        }
    }
}

void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                          const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                          NetworkInvariantCarryDriver& carry_driver,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkClockEmitRebase* clk_rebase,
                          NetworkEmitRebase* emit_rebase,
                          NetworkPbEmitFilter* pb_filter) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
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
                    if (start_offset_s <= 0.0 || (t_s - anchor_s) >= start_offset_s) {
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
                           // Card CTU G5: a snapshot written without the card's knobs is
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
                        // Restored-arc age (SPEC_event_harness §15.4): the birth stamps
                        // are not snapshotted, so the entry is stamped with the restored-arc
                        // sentinel rather than left at 0.  A 0 means "never stashed" to the
                        // harvest's first-stash-wins rule, which would re-stamp every
                        // restored arc as born at the resume.  var_first stays 0 on a file
                        // without the ratio field; the ratio ceiling then refuses the arc
                        // until it re-converges.
                        local_carry[key] = makeRestoredLocalCarry(
                            l.value_abs, l.var, l.last_seen_s, l.var_first);
                    }
                    // Seam-scoped soft refloat of the restored leveling carries (stability
                    // target 2, leg C).  The h-vintage zero-q levels were levelled under
                    // the previous run's ZWD realization; against the resume's fresh
                    // station filters they are a stiff prior of the invariant #1 kind (the
                    // post-retire ZWD drag, §18).  A one-shot variance add keeps the
                    // carried value as the prior mean (no refloat from code, no fresh
                    // station-common iono seeding) and lets the fold re-earn the
                    // stiffness.  Only the resume path reaches this code; continuous
                    // epochs do not.
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
                        // carry has no age reader and its harvest replaces the whole entry
                        // (so the sentinel does not survive the first stash), but a
                        // restored Z row is a restored entry and is marked as one, so an
                        // age gate added here later inherits §15.4.
                        zwd_carry[idx] = makeRestoredLocalCarry(l.value_abs, l.var,
                                                                l.last_seen_s);
                    // Seam-scoped ZWD re-seed (SPEC_synthetic_minimum_network §17 finding
                    // 3): arm the post-resume inject window.  This is the re-coupling edge
                    // (invariant #7): the restored h-vintage ZWD posteriors reconnect to
                    // the solve for a bounded horizon while the station filters (not
                    // snapshotted, by design) rebuild their own memory; the retire at the
                    // end of the horizon is the decoupling edge, logged at the harvest.
                    // The window uses the ordinary B9 fold, so per-entry staleness
                    // (pl_max_gap_s) and the carry_warm gate still apply per epoch.
                    if (ctx.knobs.zwd_seam_on) {
                        if (!zwd_carry.empty()) {
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
                                      zwd_carry.size(), ctx.knobs.zwd_seam_epochs,
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
                                st.ever_anchored = ca.anchored;   // card PRM phase 3a, measure-only
                                ctx.carry.clk_anchor.sat[sat] = st;
                                ++ctx.counters.n_resume_frame_cta;
                                if (ca.anchored) ++ctx.counters.n_resume_frame_cta_anchored;
                            }
                            // Prime the two-edge handover ledger from the restored
                            // leveling rows (the judge graft from card 3), so a family whose
                            // restored incumbent does not track the satellite at the first
                            // live epoch takes the continuous handover path (the
                            // NetworkPhaseDatum.cpp re-validate gate on the restored
                            // conditional variance) instead of the cold pin, 1(d) mark and
                            // erase.  The first post-resume harvest rebuilds the ledger
                            // wholesale, so this is a one-epoch seed, the same lifetime as
                            // the ordinary ledger.  Done only when the continuous handover
                            // is armed (the map is not read otherwise).
                            std::size_t n_ledger_primed = 0;
                            if (ctx.knobs.pd_handover_continuous) {
                                for (const auto& [key, lc] : ctx.carry.local_carry) {
                                    if (!(lc.var > 0.0) || !std::isfinite(lc.value_abs)) continue;
                                    AnchorHandoverLedger::Entry e;
                                    e.value = lc.value_abs;
                                    e.var   = lc.var;
                                    e.var_first = lc.var_first;
                                    ctx.carry.handover.value_abs[{std::get<0>(key),
                                        StateKeys::makeAmbiguityKey(std::get<1>(key),
                                                                    std::get<2>(key))}] = e;
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
                            // Restore also the IF-pin pair those values were solved under.
                            // Restoring the values without the pair is worse than restoring
                            // neither: `pair_same` would read false everywhere, so the IF-pin
                            // RHS would re-seed to 0, and the restored seen-set would make
                            // every one of those re-seeds bump.  A pair that did change
                            // across the seam still trips the test and bumps, because the
                            // resume's previous kept epoch is the writer's.
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
                    //
                    // The two halves are gated and reported separately, as hasFrameState /
                    // hasDatumState are: a file may carry the ER rows and no PF/PS (a
                    // writer with carry_invariant but without emit_pb_filter), and an
                    // absent half reads as inert for that half alone.
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
                    // Card CTU G5: the level information of the writer's monitor, and
                    // the raise it arms for the first leg epoch.
                    if (ctx.knobs.clm_on && !snap.level_info.empty()) {
                        for (const auto& [sat, in] : snap.level_info) {
                            ctx.carry.clm_info_c[sat] = in.first;
                            ctx.carry.clm_n[sat] = in.second;
                        }
                        ctx.carry.clm_restore_raise_pending = ctx.knobs.clm_restore_raise;
                        log->info("Network (card CTU G5): {} satellite(s) of contrast-frame level "
                                  "information restored from the snapshot's LB rows{}.",
                                  snap.level_info.size(),
                                  ctx.knobs.clm_restore_raise
                                      ? "; the level raise on restore is armed for the first "
                                        "leg epoch" : "");
                    } else if (ctx.knobs.clm_restore_raise) {
                        log->warn("Network (card CTU G5): network.clock_level_restore_raise is "
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
                            // Restored as written, without a sentinel: ShDevCarry's own cold
                            // test is `t_s <= 0`, so keeping the writer's harvest stamp makes
                            // the entry read warm, and the epoch's aging pre-pass then ages it
                            // over the true gap (and drops it at 5·tau, the staleness rule).
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
                        ctx.carry.resume_canon_rows.clear();
                        long long staged = 0;
                        for (const auto& [idx, rows] : snap.canonical_signals) {
                            auto& dst = ctx.carry.resume_canon_rows[idx];
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
                                      ctx.carry.resume_canon_rows.size());
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
                    // StateManager, and the one-shot latch that freezes it does not close
                    // until that station's first computeMeasurementUpdate, so a separate
                    // seam (networkApplyRxCbLinRestore), called from runNetwork right after
                    // this function where the station roster is in scope, consumes them.
                    // The loader requires resume_station_restore and its rxcb half, so an
                    // armed knob here always sits under a restored carry.
                    if (ctx.knobs.resume_rxcb_lin_restore) {
                        if (!snap.rx_cb_lin.empty()) {
                            ctx.carry.rxcb_lin_restore.clear();
                            for (const auto& [key, xl] : snap.rx_cb_lin)
                                ctx.carry.rxcb_lin_restore[key.first][key.second] = xl;
                            ctx.counters.n_resume_rxcb_lin_stations =
                                static_cast<long long>(ctx.carry.rxcb_lin_restore.size());
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
                            // QR: a hard-refloated satellite is skip-stashed, so restoring the
                            // set is the only way the first warm epoch's pre-pass (the
                            // prev_clock_refloat_sats loop in ApplicationNetworkAssembly.cpp)
                            // behaves as a continuous epoch's would.
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
                    // Dense D2 covariance side-car (§18.12 (2a)).  Two read knobs reach it
                    // (RS-7, §21.14).  Under resume_coupling_restore it is the other half of
                    // the coupling seam: the coupling rows are conditional from the first
                    // warm fold, so the border they condition on has to fold dense from the
                    // first fold too, or the diagonal asymmetry measured in §18.12 remains.
                    // Under resume_border_restore it stands alone: a covariance block carries
                    // no regression and no gauge identity, so it needs neither
                    // carry_conditional_coupling nor card 1f, which makes it usable on a
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
                            // Card CTU G5: the exact update's stash stamp is the snapshot's
                            // own epoch (the coupling rows restored with it were harvested
                            // there), and the clock pivot is the snapshot's (pinned at 0 in
                            // its datum).  Without this the first leg epoch has no valid
                            // stash and every row takes card 1d's kappa.
                            if (ctx.knobs.carry_cpl_exact) {
                                ctx.carry.cxu_stash_s =
                                    static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
                                ctx.carry.cxu_stash_valid = ctx.carry.prev_carry_cov.P.rows() > 0;
                                ctx.carry.cxu_pivot.clear();
                                for (const auto& [c, sat] : snap.prev_pivot)
                                    if (!sat.empty())
                                        ctx.carry.cxu_pivot[letterOfSat(sat)] =
                                            {sat, 0.0};
                                ++ctx.counters.cxu_resume_stash;
                            }
                            // Card PFQ: the restored block's frame is the snapshot's pivots.
                            if (ctx.knobs.pfq_on) {
                                ctx.carry.pfq_pivot.clear();
                                for (const auto& [c, sat] : snap.prev_pivot)
                                    if (!sat.empty()) ctx.carry.pfq_pivot[letterOfSat(sat)] = sat;
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
                    // The same window one layer up, on the estimation-side fine-slip gate
                    // (RS-2b, §21.6).  It is armed here and nowhere else: a cold start never
                    // sets it, since the gate would otherwise be diagnostic-only on a run
                    // with no restored branch to protect.  The countdown is decremented at
                    // the install seam.
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
                    // Restore the eph-cutover re-level baseline and any pending forced
                    // disc, so a warm resume does not re-level against a stale baseline (or
                    // lose a pending forced bump).  Both are empty in an sp3 snapshot or one
                    // written before card 6, and the first brdc_rts epoch then re-seeds.
                    relevel_iode             = snap.relevel_iode;
                    eph_cutover_disc_pending = snap.eph_cutover_disc_pending;
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
                    // anchor_age_resume_seed and adm_resume_pending).
                    ctx.carry.resume_datum_first_ep = true;
                    // One-shot latch of the declaration half (RS-4, §21.9), set beside the
                    // RS-3 telemetry latch and for the same reason: every warm resume arms
                    // it, so an A/B's control leg is read by the same instrument.
                    // NetworkEmitStore consumes it at the first emit, the only point at
                    // which the emitter's restored per-key baselines still hold the previous
                    // run's emitted values.  Read only under the knob.
                    ctx.carry.resume_pb_seam_pending = true;
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
                    // opens, standing invariant #1).  The census card's INC-3 cannot see
                    // this case by construction, which is why the spec names it.  Written
                    // unconditionally (inert unless ctx.knobs.adm_screen), so the off path
                    // stays byte-identical, as for anchor_age_resume_seed.
                    ctx.carry.adm_resume_pending = true;
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
                        ctx.carry.carry_sat_vertex = snap.carry_sat_vertex;
                        ctx.carry.carry_group_id.clear();
                        for (const auto& gg : snap.carry_group_id)
                            ctx.carry.carry_group_id[{std::get<0>(gg), std::get<1>(gg)}] =
                                std::get<2>(gg);
                        std::istringstream cs(snap.carry_state);
                        std::string cerr;
                        bool carry_ok = carry_driver.restoreCarry(cs, &cerr);
                        if (carry_ok) {
                            // The blob is numbered in the writing run's id space, so it is
                            // rejected unless every chord and arc binds to these live maps,
                            // instead of re-keying chords onto the wrong satellites (RE-4).
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
                            // Event response for the stiff restored carry (RE-2).
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
                                      // A diagonal restore looks identical to a dense one
                                      // everywhere else (RTB-7(e), invariant #4).
                                      carry_driver.resumeCarryDiagonal()
                                          ? fmt::format("DIAGONAL-ONLY (v3 blob, {} block(s)) — "
                                                        "means exact, chord cross-covariances "
                                                        "start at ZERO and re-earn from data",
                                                        carry_driver.resumeCarryDiagonalBlocks())
                                          : std::string("dense (v1/v2 blob) — the branch "
                                                        "geometry resumes exactly"),
                                      // Whether the blob carries ZL rows decides whether
                                      // resume_carry_rebase has anything to act on, and
                                      // nothing else in this line shows it (RS-2,
                                      // invariant #4).
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
                            // Armed with nothing to act on (RS-2, invariant #4): the run reads
                            // "warm + branch-faithful" and the re-base counters stay 0, and
                            // this warning states why.
                            if (config_.networkResumeCarryRebase()
                                && carry_driver.resumeCarryLinBlocks() == 0)
                                log->warn("Network (RS-2): network.resume_carry_rebase is armed "
                                          "but the carry blob carries NO ZL rows — the branch "
                                          "is NOT re-based and this resume keeps today's "
                                          "behaviour (every restored chord folds against the "
                                          "WRITER's x_lin).  Re-write the seed with "
                                          "network.state_snapshot_carry_lin.");
                            // Reporting half of the PD-safe restore guard (INC-2c).  A blob
                            // written without pd_safe can carry a corrupt joint lane block
                            // (indefinite Pᶻ, negative diagonals); importing it verbatim would
                            // seed the fixed build with the frozen carry the fix releases.  A
                            // non-zero count on the first pd_safe resume is expected; a
                            // non-zero count when resuming from a pd_safe-armed run is a new
                            // defect, because the writing run's blocks were PD by construction.
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
    auto& staged = ctx.carry.resume_canon_rows;
    if (staged.empty()) return;

    long long n_sta = 0, n_ent = 0, n_orphan = 0;
    for (const auto& [idx, rows] : staged) {
        if (idx < 0 || idx >= static_cast<int>(stations.size())
            || !stations[static_cast<std::size_t>(idx)].model) {
            // Unreachable when the fingerprint passes (the roster is one of its terms),
            // so this is a tripwire rather than a handled case, and the drop is counted
            // and logged below rather than skipped without a trace.
            ++n_orphan;
            continue;
        }
        const std::size_t applied =
            stations[static_cast<std::size_t>(idx)].model->restoreCanonicalSignals(rows);
        if (applied == 0) continue;
        ++n_sta;
        n_ent += static_cast<long long>(applied);
    }
    ctx.counters.n_resume_canon_stations = n_sta;
    ctx.counters.n_resume_canon_entries  = n_ent;
    staged.clear();   // one-shot: the latch is written, the buffer owns nothing further

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
// initializers (RS-9, SPEC_event_harness §21.18).
//
// This is a separate seam rather than a block inside networkTryWarmResume because
// the two need different inputs: the resume function owns the snapshot and stashes
// the RL map into ctx.carry, while this one needs the mutable station roster.  The
// install matters only between the resume and the first computeMeasurementUpdate,
// since StateInitializer::isCodeBiasInitialized is a one-shot latch, so the driver
// calls it immediately after the resume, in the same way as the RS-8
// canonical-signal restore.
// ---------------------------------------------------------------------------
void networkApplyRxCbLinRestore(NetworkRunContext& ctx, const GnssConfig& config_,
                                std::vector<NetStation>& stations,
                                const std::shared_ptr<spdlog::logger>& log) {
    // Nothing stashed ⇒ nothing to do: the knob is off, no warm resume happened, or
    // the snapshot carried no RL rows (warned at the resume).  No allocation and no
    // branch taken inside any station, so the run is byte-identical.
    if (ctx.carry.rxcb_lin_restore.empty()) return;
    (void)config_;   // the knob is already resolved into ctx.knobs / the stash
    // One out-slot pair per station, sized once here and never resized: each
    // StateInitializer keeps a raw pointer to its own element and increments it from
    // inside the OpenMP parallel produce phase.  Distinct objects ⇒ race-free without
    // atomics, the same discipline as the per-station StationAssembly tallies.  The
    // pair is {applied, carried-but-unseeded}; the second is scoped to the keys the
    // map carries (the RL rows pair 1:1 with the RC rows), so a code-bias family with
    // no carry, whose x_lin no gate reads, is not counted as a miss.
    ctx.carry.rxcb_lin_sta_counts.assign(stations.size(), {0LL, 0LL});
    long long installed_sta = 0, installed_entries = 0, orphan = 0;
    for (const auto& [sidx, keymap] : ctx.carry.rxcb_lin_restore) {
        if (sidx < 0 || sidx >= static_cast<int>(stations.size())) { ++orphan; continue; }
        auto& st = stations[static_cast<std::size_t>(sidx)];
        if (!st.mm) { ++orphan; continue; }
        auto& slot = ctx.carry.rxcb_lin_sta_counts[static_cast<std::size_t>(sidx)];
        st.mm->getStateInitializer().setCodeBiasSeedOverride(keymap, &slot.first,
                                                             &slot.second);
        ++installed_sta;
        installed_entries += static_cast<long long>(keymap.size());
    }
    // The roster is inside the warm-resume fingerprint, so an index that does not
    // resolve should be unreachable; if one occurs, the restore for that station is
    // lost, and the warning makes it visible (invariant #4).
    if (orphan > 0)
        log->warn("Network (RS-9 rx-CB frame restore): {} restored station index(es) did "
                  "not resolve to a live station model and were DROPPED — those stations "
                  "re-derive their code-bias linearization point from the resume epoch's "
                  "code, which is the pre-card behaviour for them.", orphan);
    log->info("Network (RS-9 rx-CB frame restore): installed on {} station(s), {} "
              "linearization point(s) — consumed at each station's first-epoch code-bias "
              "initialization (the one-shot isCodeBiasInitialized latch), so the rx-CB "
              "inject gate's reference is the writer's rather than this leg's.",
              installed_sta, installed_entries);
    // Consumed: the map has been handed to the initializers, and re-installing it
    // later would be inert (the latch has closed).  Clearing it here enforces that.
    ctx.carry.rxcb_lin_restore.clear();
}

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

void networkInjectShVtecPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                              const GnssConfig& config_, NetworkEstimator& est,
                              const SphericalHarmonicIonosphere* sh_model,
                              const SphericalHarmonicIonosphere* sh_model2,
                              bool datum_changed, double dt, bool within_epoch_armed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    auto& prev_carry = ctx.carry.prev_carry;
    auto& cap_priors = ws.cap_priors;

    // --- SH-VTEC prior (Kaula + temporal), folded before the gate -----------
    // The SH coefficients are shared columns in the reduced normals.  Without a bound
    // on each one, a coefficient whose IPP support has rotated over an ocean or polar
    // gap is near-null and trips the all-or-nothing rank gate.  A degree-dependent
    // Kaula prior (σ_n = kaula_scale/n; a₀₀ gets a loose climatology prior, so the mean
    // VTEC stays data-determined and is not clamped to an external value) keeps every
    // coefficient PSD-bounded, so declared_dimension stays = #constellations regardless
    // of coverage.  The same prior carries the field forward in time as a bounded
    // random walk (anchor the previous estimate, inflate by the per-degree q, never
    // exceed the Kaula bound).  It is folded through addSharedPrior before the gate, so
    // the gate sees the regularized SH block; the clock/bias D2 carry-forward, by
    // contrast, stays after the gate, because it would otherwise fill the clock gauge
    // that the datum pins.
    if (config_.estimateNetworkIonoSh() && sh_model && est.numShared() > 0) {
        std::map<std::tuple<int, int, int, char>, const CarryPrior*> sh_carry;
        // Scope decision (RTA-2): the SH-VTEC carry skip stays global, on purpose, even
        // under network.scoped_handover_response.  The SH coefficients are shared
        // columns without a constellation, so no scoped subset exists for them; and
        // although the pure clock-gauge S-transform leaves the iono columns invariant in
        // exact arithmetic, the carried SH posterior was estimated jointly with the
        // old-datum clock/bias states.  The conservative response is therefore kept: on
        // any datum change the random-walk-toward-previous term is dropped for one epoch
        // and the coefficients fall back to the always-on Kaula climatology anchor below
        // (still PSD-bounded and gate-safe), a small, bounded cost that avoids scoping a
        // column that belongs to no constellation.
        if (!datum_changed)
            for (const CarryPrior& cp : prev_carry)
                if (cp.kind == SharedParamKind::VtecSh)
                    sh_carry.emplace(
                        std::make_tuple(cp.sh_layer, cp.sh_n, cp.sh_m, cp.sh_trig), &cp);
        // Card PRM phase 3a: the carried SH entries this datum change leaves unfolded this
        // epoch (key-epochs; a skipped epoch keeps them for the next).
        if (datum_changed) {
            long long prm_n_sh = 0;
            for (const CarryPrior& cp : prev_carry)
                if (cp.kind == SharedParamKind::VtecSh) ++prm_n_sh;
            prmRespond(ctx.counters.prm_resp, ResponseSite::ShRetire, prm_n_sh);
        }

        // Dual-layer (Iono-2): the same mean-reverting prior recipe per shell, with a
        // per-layer Kaula scale, q and a₀₀ anchor.  The layer-2 a₀₀ anchor is tight on
        // purpose: the inter-layer split of the mean VTEC is nearly null from
        // ground-only geometry (only the mapping-function difference of the two shell
        // heights observes it), while the total mean stays data-determined through
        // layer 1's loose anchor.
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

                // Mean-reverting (first-order Gauss-Markov) prior: always apply the
                // Kaula climatology anchor (toward clim, σ = kaula_sigma) and, when a
                // posterior was carried forward, a random-walk anchor (toward the
                // previous estimate, inflated by the per-degree q).  Combining them in
                // information space pulls every coefficient back toward climatology
                // each epoch, so the field cannot random-walk away (the design's
                // "unobserved coefficients are pulled to their prior", §6.1), while the
                // random-walk term keeps temporal continuity for well-observed ones.  A
                // pure random walk toward the previous value, with no climatology
                // term, lets the global field drift to unphysical magnitudes; the
                // climatology term bounds it.
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
        est.contributionKind(ContributionKind::ShPrior);   // card NOM N2a
        est.addSharedPrior(sh_priors);
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, sh_priors,
                                 ContributionKind::ShPrior);
    }
}

void networkInjectPhaseHoldPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                 NetworkEstimator& est, bool within_epoch_armed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    const bool   phase_hold_on = ctx.knobs.phase_hold_on;
    const double phase_hold_var = ctx.knobs.phase_hold_var;
    auto& cap_priors = ws.cap_priors;

    // --- Fix A hold: held satellite phase-bias datum (seed once, then time-locked) ---
    // Inject the held b_s priors before the null-space gate, like the SH-VTEC prior
    // above and unlike the clock/code D2 carry below (which stays after the gate so
    // the gate sees the data-only clock-gauge structure).  A held (sat,signal) has its
    // Σz=0 fold skipped in buildEnsembleGauges (a hard w→∞ fold would override a
    // finite prior), so this stiff fixed-variance prior is that column's datum.  It
    // has to be in the reduced normals the gate inspects, or the held column reads as
    // a near-null gauge and triggers the O(n³) null-space repair or a withhold every
    // epoch.  (The tight local ambiguity carry usually lifts it above the cutoff
    // already, but this makes the datum explicit and robust to a weak or cold carry on
    // a fresh arc.)  The phase-ensemble datum is orthogonal to the clock-pivot
    // datum_changed handover, so it is injected every epoch regardless.  Under hold,
    // phase biases are excluded from the clock-frame D2 carry loops below, so this is
    // the only injection (no double fold).  No-op unless phase_hold_on
    // (byte-identical).
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
        est.contributionKind(ContributionKind::PhaseHold);   // card NOM N2a
        est.addSharedPrior(hp);  // skips keys absent from this epoch's layout
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, hp,
                                 ContributionKind::PhaseHold);
    }
}

void networkInjectPinBridgePrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                 NetworkEstimator& est, bool within_epoch_armed,
                                 const std::shared_ptr<spdlog::logger>& log) {
    // --- eq34 reattach hygiene (INC-2): the time-linked phase datum ----------
    // The header explains why this is folded before the gate.  Inert (no allocation,
    // no row) unless a bridge is open, which requires network.phase_datum_time_linked.
    auto& bridges = ctx.carry.pin_bridge;
    if (!ctx.knobs.pin_bridge_on || bridges.empty()) return;
    std::vector<NetworkEstimator::SharedPrior> bp;
    bp.reserve(bridges.size());
    for (auto& [amb, br] : bridges) {
        // "sat_signal" → (sat, signal).  The satellite id never contains '_', so the
        // first underscore splits it (an iono-free combination keeps its own
        // underscores inside `signal`).
        const auto parts = StateKeys::parseAmbiguityKey(amb);
        if (!parts) continue;
        if (!std::isfinite(br.value)) continue;
        NetworkEstimator::SharedPrior p;
        p.kind   = SharedParamKind::PhaseBias;
        p.sat    = parts->satellite;
        p.signal = parts->signal;
        p.value  = br.value;
        p.info   = ctx.knobs.pin_bridge_info;
        bp.push_back(p);
        ++br.folds;                       // staleness clock (bounded by bridge_max_ep)
        ++ctx.counters.hyg.bridge_folds;
    }
    if (bp.empty()) return;
    est.contributionKind(ContributionKind::PinBridge);   // card NOM N2a
    est.addSharedPrior(bp);   // skips keys absent from this epoch's layout
    if (within_epoch_armed)
        networkCapturePriors(ws.cap_priors, ws.cap_prior_kinds, bp,
                             ContributionKind::PinBridge);
    // Margin check, sampled once (the analogue of card KP's K2): a datum row whose
    // information sits at the rank-gate cutoff either trips the gate or survives by
    // coincidence.  The bridge is a datum row, so it has to sit well above the cutoff;
    // the actual margin is reported rather than assumed from the default sigma.  The
    // reduced normals here already contain the row just folded, which is what the gate
    // is about to judge.
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

// READMIT continuity: whether a parked entry's shared column is registered in this epoch's
// layout is carryColumnPresent (filter/NetworkCarryDisposition, card PRM phase 1b), the one
// predicate the fold's decision and the stash's park share.

/// READMIT continuity: bookkeeping and the re-coupling-edge event for one column seeded
/// from a parked pre-gap realization at the D2 install (standing invariant #7: the event
/// records the edge's identity, its evidence gap, and the variance it re-coupled at).
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
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_carry_cov = ctx.carry.prev_carry_cov;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    const bool   fullcov_on = ctx.knobs.fullcov_on;
    // The per-key rules (the phase hold, the eq34 and anchored-election re-reference sets,
    // the clock oscillator, the registry's process noise, the reinit variance, the B4
    // floors and the VCE apply factor) are in the decision made before PASS B
    // (filter/NetworkCarryDisposition, card PRM phase 1b), whose carryFoldVarEff is the one
    // chain both branches below fold through, so the diagonal and the C1 dense fold cannot
    // drift apart: the dense branch's diagonal is var_eff, and the marginal prior on any one
    // state has to be identical either way.
    // Card PRM phase 1b: the per-key read of the decision made once before PASS B; see
    // prmFoldKey below, after the predicates and the epoch clocks it reads.
    bool prm_reexpressed_epoch = false;
    const auto prmTally = [&](CarryPrior& cp, CarryDisposition disp) {
        cp.fold.disposition = disp;
        const int kk = static_cast<int>(cp.kind);
        ++ctx.counters.prm_disp[kk][static_cast<int>(disp)];
        if (carryDispositionFolds(disp) && cp.fold.departed) ++ctx.counters.prm_departed[kk];
    };
    auto& cap_priors = ws.cap_priors;
    auto& cap_dense_keys = ws.cap_dense_keys;
    auto& cap_dense_cov = ws.cap_dense_cov;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // Card SCL: which clones carry row information, read before any prior is folded on a
    // clone column (filter/NetworkClockClone).  No-op off.
    networkClockCloneFoldBegin(ctx, est);
    // The guarded temporal clock anchor sets the clock rows' fold variance in both
    // branches below.  The per-satellite elapsed time it inflates over is continuous
    // GPS seconds, not the driver's inter-epoch dt (see the seam header: every skip path
    // advances dt while the carry survives).  Untouched and unallocated when off.
    const bool   cta_on    = ctx.knobs.cta_on;
    const double cta_now_s = cta_on ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (cta_on) clockAnchorBeginEpoch(ctx);
    // Scope predicate (RTA-2), re-declared over the explicit (datum_changed,
    // changed_consts) parameters, with the same read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };
    // The half of the S-transform (queue 1(b), INC-1) that changes the solve.  When the
    // transform re-expressed this constellation's carry (ctx.carry.sts_consts, filled by
    // networkHandoverStransform earlier this epoch), the prior is valid in the incoming
    // datum and folds: the retired-datum skip below is released for exactly those keys,
    // and the retained rows are counted (invariant #4: transforms applied with
    // sts_rows_retained == 0 mean this release is mis-wired).  The set is empty on every
    // epoch without a handover and whenever the knob is off, so the off path is
    // byte-identical.
    const auto sts_released = [&](const std::string& sat) {
        return !ctx.carry.sts_consts.empty() && !sat.empty()
            && ctx.carry.sts_consts.count(letterOfSat(sat)) > 0;
    };
    // READMIT continuity (E2 seed-first re-admission, network.readmit_seed_horizon_s).
    // Set up the epoch clock that the stash (which has none of its own) and the guard
    // read, clear this epoch's seed check list, and apply the frame drop: a clock-datum
    // change retires a parked entry's frame as it skips the live carry of the changed
    // constellations, but a parked entry cannot be re-stashed fresh next epoch, so it is
    // dropped (and counted) rather than skipped.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    const double now_s = (readmit_h > 0.0) ? TimeUtils::continuousGpsSeconds(t) : 0.0;
    if (readmit_h > 0.0) {
        ctx.carry.readmit_now_s = now_s;
        ctx.carry.readmit_seed_check.clear();
        if (datum_changed) {
            const auto before = prev_carry.size();
            const auto parked_retired = [&](const CarryPrior& cp) {
                return cp.t_park_s > 0.0
                    && handover_affects(cp.sat)
                    && !sts_released(cp.sat);
            };
            // Card PRM phase 3a: the parked entries the drop below removes, per kind (the
            // same pure predicate, read-only, ahead of the erase).
            for (const CarryPrior& cp : prev_carry) {
                if (!parked_retired(cp)) continue;
                if (cp.kind == SharedParamKind::Clock)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::ClkParkDiscardDatum);
                else if (cp.kind == SharedParamKind::CodeBias)
                    prmRespond(ctx.counters.prm_resp, ResponseSite::CbParkDiscardDatum);
            }
            prev_carry.erase(std::remove_if(prev_carry.begin(), prev_carry.end(),
                                            parked_retired),
                             prev_carry.end());
            ctx.counters.n_readmit_dropped_datum +=
                static_cast<long long>(before - prev_carry.size());
        }
    }
    // Card PRM phase 1b: both branches read each entry's fold decision, made once before
    // PASS B by networkCarryDecide (filter/NetworkCarryDisposition) and read by the coupling
    // rows too: the clauses in the fold's order (the SH prior and the phase hold fold their
    // keys elsewhere; a frame re-reference, the clock oscillator and a park do not fold; a
    // monitor-flagged clock folds at the reinit variance; a seed at q over the gap since its
    // park; a clock under the Q3 anchor at the anchor's increment), the B4 floors, the VCE
    // factor and the switch-on seam's level noise (dense branch).  The fold adds the clause
    // no earlier point can know: a clock-pivot handover retires the changed constellations'
    // datum unless the S-transform re-expressed their carry (recorded as Retired).  It forms
    // the variance on the carried variance it holds, which the S-transform has changed since
    // the rows were built (counted as re-expressed), commits the anchor's per-satellite
    // bookkeeping, records a non-positive variance as Screened, and tallies what it applied.
    // Returns true with the variance to fold; false when the entry does not fold.
    const auto prmFoldKey = [&](CarryPrior& cp, bool dense, double& var_eff,
                                bool& seed_anchored) -> bool {
        if (cp.fold.disposition == CarryDisposition::Undecided) {   // no pass this epoch
            ++ctx.counters.prm_undecided_at_fold;
            cp.fold = carryFoldDecide(ctx, est, cp, dt, cta_now_s, now_s);
        }
        const CarryFoldDecision& d = cp.fold;
        // Fix A hold / the SH prior: folded elsewhere, before the gate.  The phase-ensemble
        // datum is orthogonal to the clock pivot handover this clock-frame carry is gated
        // on, so a clock datum change does not drop a held phase bias (otherwise a held
        // satellite, whose sum-to-zero fold buildEnsembleGauges skipped, would be left
        // without a datum and the epoch would withhold).
        if (d.disposition == CarryDisposition::Held) {
            prmTally(cp, CarryDisposition::Held);
            return false;
        }
        if (handover_affects(cp.sat)) {
            if (!sts_released(cp.sat)) {               // retired-datum key, scoped mode only (RTA-2)
                if (dense && ctx.knobs.carry_cpl_exact && ctx.carry.cxu_epoch.ok) {
                    const CouplingExactEpoch& xe = ctx.carry.cxu_epoch;
                    const auto pk = xe.p_of_key.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                    if (pk != xe.p_of_key.end()
                        && xe.s_of_p[static_cast<std::size_t>(pk->second)] >= 0)
                        ++ctx.counters.cxu_fold_retired_in_s;   // the rows carried it in S
                }
                prmTally(cp, CarryDisposition::Retired);
                return false;
            }
            ++ctx.counters.sts_rows_retained;          // re-expressed => folds instead (queue 1(b))
        }
        // A re-referenced phase-bias frame (eq34 D4 hygiene, card 3; the anchored election,
        // queue item 1(d)): marginalized for one epoch, its prior belongs to the retired
        // frame.  A clock on the clock oscillator; a parked entry (column absent, or past
        // the seed horizon).
        if (!carryDispositionFolds(d.disposition)) {
            prmTally(cp, d.disposition);
            return false;
        }
        // The satellite clock's temporal constraint commits here, at the fold, so an epoch
        // that ends earlier advances nothing; a monitor-driven reinit is excluded (a stiff
        // anchor would re-freeze what the monitor just loosened).
        ClockAnchorFold cf;
        var_eff = carryFoldVarEff(ctx, cp, d, cp.var, dense, &cf);
        seed_anchored = false;
        if (cta_on && cp.kind == SharedParamKind::Clock
            && d.disposition != CarryDisposition::Reinit) {
            clockAnchorFoldCommit(ctx, cp.sat, cf, cta_now_s, cp.value);
            seed_anchored = cf.anchored;
        }
        if (cp.var != d.var_decided_m2 || cp.value != d.value_decided_m) {
            ++ctx.counters.prm_reexpressed;
            prm_reexpressed_epoch = true;
            ctx.counters.prm_reexpressed_max_dvar = std::max(
                ctx.counters.prm_reexpressed_max_dvar, std::abs(cp.var - d.var_decided_m2));
        }
        if (!(var_eff > 0.0)) {
            prmTally(cp, CarryDisposition::Screened);
            return false;
        }
        prmTally(cp, d.disposition);
        return true;
    };

    // --- Diagonal D2 carry-forward prior (inflated over the gap) --------
    // Applied after the gate (which has to see the data-only gauge structure) and before
    // the solve.  The prior anchors each non-datum shared parameter to its
    // previous-epoch estimate; it is skipped on a pivot handover, when the carried values
    // belong to a retired datum.  Under network.scoped_handover_response (RTA-2) only the
    // changed constellations' datum is retired: a per-constellation clock gauge is
    // independent of its siblings, so carries of unaffected constellations remain valid
    // and fold normally, and the per-key skip below drops exactly the retired-datum keys.
    // Off reproduces the legacy all-or-nothing skip byte for byte (the outer gate reduces
    // to !datum_changed, and the per-key skip cannot trigger inside it).
    if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on) {
        std::vector<NetworkEstimator::SharedPrior> priors;
        priors.reserve(prev_carry.size());
        for (CarryPrior& cp : prev_carry) {
            // The entry's decision (card PRM phase 1b, prmFoldKey above), which holds the
            // clauses this loop applied before it.  READMIT (E2 seed-first re-admission): a
            // parked entry is a departed satellite's pre-gap realization; with its column
            // re-registered it seeds the column at var + q*gap, the Q3 anchor folds over its
            // own per-satellite elapsed time bounded by the seed horizon, and the guard
            // re-validates.  A clock-monitor-flagged satellite folds with the large reinit
            // variance (a soft large-q reinitialization): the weak prior still anchors the
            // parameter at its previous value, which limits the collective datum drift a hard
            // refloat causes, but is loose enough for the data to move a frozen clock back
            // toward truth.  The B4 floor of the phase-bias / code-bias carry caps the
            // information so a stiff zero-q carry cannot lock in a biased product.
            double var_eff = 0.0;
            bool seed_anchored = false;
            if (!prmFoldKey(cp, /*dense=*/false, var_eff, seed_anchored)) continue;
            const double seed_gap = cp.fold.seed_gap_s;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;
            if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                clk_prior_info[cp.sat] = p.info;   // scalar clock carry info on this column (B7)
            priors.push_back(p);
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
        }
        // With the gauge-free fold armed (card Q3 §2.1), the clock entries are folded
        // with the per-constellation common direction marginalized out, so the anchor
        // constrains the estimable between-satellite differences and leaves the gauge to
        // the S-basis datum.  Non-clock entries fold identically either way, and the off
        // path is the original single call.
        const ContributionKind carry_kind = ctx.knobs.carry_gauge_free
                                                ? ContributionKind::BorderCarryGaugeFree
                                                : ContributionKind::BorderCarryDiagonal;
        est.contributionKind(carry_kind);   // card NOM N2a
        if (ctx.knobs.carry_gauge_free) {
            const std::size_t gf = est.addSharedPriorClockGaugeFree(
                priors, ctx.knobs.carry_gauge_free_closure);
            ctx.counters.cta_gf_keys += static_cast<long long>(gf);
            if (gf == 0) ++ctx.counters.cta_gf_inert;
        } else {
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
        }
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, priors, carry_kind);
    } else if ((!datum_changed || scoped_handover) && !prev_carry.empty()) {
        // Dense carry (C1, network.shared_carry_full_cov): fold the carried block as one
        // dense information prior Λ = (P_prev + Q)⁻¹, the information-form KF time update
        // on the shared border, which keeps the cross-covariances (most importantly
        // clock↔phase-bias) that the diagonal branch above discards.  Entry selection,
        // per-key skip rules and the per-key var_eff adjustments (inflate_q reinit, q·dt
        // inflation, B4 floors) are identical to the diagonal branch; the only addition
        // is the retained off-diagonals.  Keys with no matrix row (the first fold after a
        // warm resume, or after a stash-side non-finite screen) fold through the legacy
        // diagonal path, so the mode degrades per key and does not fail.  Keys skipped by
        // rule are marginalized (their rows and columns of P are not selected), which is
        // the exact treatment for a departed or retired parameter.
        std::vector<NetworkEstimator::SharedPrior> dkeys;     // dense-eligible set
        std::vector<std::size_t> drow;                        // row in prev_carry_cov.P
        std::vector<double> dvar_eff;                         // per-key var_eff (diag + fallback)
        std::vector<char> dinflate;                           // inflate_q per dense key
        std::vector<char> dreexp;                             // re-expressed since the decision (card PRM phase 1b)
        std::vector<double> dpfq_rate, dpfq_dt;               // card PFQ: the pivot's rate and the key's interval
        std::vector<NetworkEstimator::SharedPrior> leftovers; // no matrix row → diagonal fold
        dkeys.reserve(prev_carry.size());
        drow.reserve(prev_carry.size());
        dvar_eff.reserve(prev_carry.size());
        dinflate.reserve(prev_carry.size());
        dreexp.reserve(prev_carry.size());
        for (CarryPrior& cp : prev_carry) {
            // The entry's decision (card PRM phase 1b, prmFoldKey above), the same per-key
            // rules as the diagonal branch, so the marginal prior on any one state is
            // identical on both branches (the property that makes the guard statistic
            // path-independent; the dense fold's off-diagonals are kept on purpose, so the
            // anchor does not re-create the independence assumption C1 removed on the state
            // it makes stiffest, SPEC section 2.1).  A READMIT seed has no dense row (it was
            // never in the stashed block), so it folds through the diagonal leftovers below.
            // Card CTU G5: a switch-on seam's level noise is on the diagonal (inside the
            // decision's variance) and between the satellite's clock and phase-bias keys
            // below.
            double var_eff = 0.0;
            bool seed_anchored = false;
            if (!prmFoldKey(cp, /*dense=*/true, var_eff, seed_anchored)) continue;
            const double seed_gap = cp.fold.seed_gap_s;
            NetworkEstimator::SharedPrior p;
            p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
            p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
            if (seed_gap > 0.0)
                readmitNoteSeed(ctx, traces, t, cp, seed_gap, readmit_h, var_eff, seed_anchored);
            const auto rit = prev_carry_cov.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (rit == prev_carry_cov.row.end()) {
                if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                    clk_prior_info[cp.sat] = p.info;   // diagonal-carry info, no dense row (B7)
                leftovers.push_back(p);
                continue;
            }
            dkeys.push_back(p);
            drow.push_back(rit->second);
            dvar_eff.push_back(var_eff);
            dinflate.push_back(cp.inflate_q ? 1 : 0);
            dreexp.push_back((cp.var != cp.fold.var_decided_m2 || cp.value != cp.fold.value_decided_m) ? 1 : 0);
            dpfq_rate.push_back(cp.fold.pfq_rate_m2ps);
            dpfq_dt.push_back(cp.fold.dt_applied_s);
        }
        const std::size_t kd = dkeys.size();
        // Card CTU: the coupling rows were built in PASS B from the same decision
        // (networkCouplingExactPrepare reads it, card PRM phase 1b).  A key in S that a
        // clock-pivot handover re-expressed after the rows were built folds here on the
        // re-expressed variance, while its rows were conditioned on the decided one; it is
        // counted (the retired ones are counted in prmFoldKey).  Read-only.
        if (ctx.knobs.carry_cpl_exact && ctx.carry.cxu_epoch.ok) {
            const CouplingExactEpoch& xe = ctx.carry.cxu_epoch;
            if (datum_changed) ++ctx.counters.cxu_fold_handover_epochs;
            for (std::size_t a = 0; a < kd; ++a) {
                const std::size_t p = drow[a];
                if (p < xe.s_of_p.size() && xe.s_of_p[p] >= 0 && dreexp[a])
                    ++ctx.counters.cxu_fold_reexpressed_in_s;
            }
        }
        if (kd > 0) {
            // P_eff over the dense set: the subset of the stashed posterior covariance,
            // with diagonal := var_eff (the stashed diagonal equals cp.var by
            // construction, so this is the legacy q·dt inflation plus the B4 floor).  A
            // clock-monitor-flagged key is decorrelated before its diagonal becomes the
            // reinit variance: its old cross-covariances are stale once the variance is
            // replaced, and a swapped diagonal under live off-diagonals could make P_eff
            // indefinite.
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
            // Card CTU G5: the off-diagonal half of the seam's level noise (its diagonal is
            // in dvar_eff): v between every pair of the satellite's clock and phase-bias
            // keys, so the phase-visible combinations get no noise.
            if (!ctx.carry.clm_seam_q.empty()) {
                const auto lvl = [&](std::size_t a) {
                    const auto& k = dkeys[a];
                    return !dinflate[a] && (k.kind == SharedParamKind::Clock
                                            || k.kind == SharedParamKind::PhaseBias);
                };
                for (std::size_t a = 0; a < kd; ++a) {
                    if (!lvl(a)) continue;
                    const auto sq = ctx.carry.clm_seam_q.find(dkeys[a].sat);
                    if (sq == ctx.carry.clm_seam_q.end()) continue;
                    for (std::size_t b = 0; b < kd; ++b)
                        if (b != a && lvl(b) && dkeys[b].sat == dkeys[a].sat)
                            cov_eff(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) += sq->second;
                }
            }
            // Card PFQ: the pairwise half of the pivot-frame clock noise.  The diagonal half
            // is inside dvar_eff through the decision; two clocks of one constellation that
            // took it share the pivot's walk over the overlap of their intervals.  A reinit
            // key took no term (its rate is 0), so its decorrelated row stays zero.
            if (ctx.knobs.pfq_on) {
                for (std::size_t a = 0; a < kd; ++a) {
                    if (!(dpfq_rate[a] > 0.0) || dkeys[a].kind != SharedParamKind::Clock) continue;
                    for (std::size_t b = a + 1; b < kd; ++b) {
                        if (!(dpfq_rate[b] > 0.0) || dkeys[b].kind != SharedParamKind::Clock
                            || systemOfSat(dkeys[b].sat) != systemOfSat(dkeys[a].sat))
                            continue;
                        const double v = pivotFrameCross(dpfq_rate[a], dpfq_dt[a], dpfq_dt[b]);
                        const auto ia = static_cast<Eigen::Index>(a), ib = static_cast<Eigen::Index>(b);
                        cov_eff(ia, ib) += v;
                        cov_eff(ib, ia) += v;
                        ++ctx.counters.pfq.dense_pairs;
                    }
                }
            }
            try {
                Eigen::VectorXd lam;
                // Card SCL: the joint fold over the stand-ins (each cloned clock stood in for
                // by its clone) and the clones' walk factors when a clone stands in this
                // epoch, which also writes the B7 information; card CTU's fold otherwise.
                // Off, the clone fold returns false at once and the fold below is unchanged.
                const bool ccl_fold = networkClockCloneDenseFold(
                    ctx, ws, est, dkeys, drow, dvar_eff, dinflate, dreexp, cov_eff,
                    within_epoch_armed, anchor_info_trace.is_open() ? &clk_prior_info : nullptr);
                if (!ccl_fold) {
                    est.contributionKind(ContributionKind::BorderCarryDense);   // card NOM N2a
                    est.addSharedPriorDense(dkeys, cov_eff,
                                            anchor_info_trace.is_open() ? &lam : nullptr,
                                            ctx.knobs.carry_gauge_free,
                                            ctx.knobs.carry_gauge_free_closure);
                    if (anchor_info_trace.is_open())
                        for (std::size_t a = 0; a < kd; ++a)
                            if (dkeys[a].kind == SharedParamKind::Clock)
                                clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // folded Λ(c,c) (B7)
                }
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
                // Numerical fallback: fold the same keys as independent diagonal priors
                // with 1/var_eff, the legacy semantics for this epoch.  The next emit
                // re-stashes a fresh block, so the fallback lasts one epoch.
                log->warn("Network (C1): dense carried-covariance fold failed ({}) — "
                          "falling back to the diagonal carry for this epoch.",
                          e.what());
                traces.emitEvent(t, "D2_DENSE", "fallback_diagonal", "", "",
                          static_cast<double>(kd), std::nullopt, "fallback",
                          e.what());   // non-PD, non-finite or conditioning budget; emitEvent sanitizes commas
                // Card CTU: the rows of this epoch were updated against the dense block.
                if (ctx.knobs.carry_cpl_exact) ++ctx.counters.cxu_fold_diag_fallback;
                networkClockCloneNoteFallback(ctx);   // card SCL: the clones are then free or isolated
                for (std::size_t a = 0; a < kd; ++a) {
                    if (anchor_info_trace.is_open() &&
                        dkeys[a].kind == SharedParamKind::Clock)
                        clk_prior_info[dkeys[a].sat] = dkeys[a].info;
                    leftovers.push_back(dkeys[a]);
                }
            }
        }
        // First post-resume fold (§18.12, invariant #7): unless the dense border was
        // restored from the snapshot's QK/QP records (RS-7, see networkTryWarmResume),
        // every carried key misses prev_carry_cov.row here and folds through the legacy
        // diagonal path while the restored coupling rows are already conditional.  That
        // asymmetry lasts one epoch; the census measures its size instead of assuming
        // it.  It is read here, before the gauge-free / plain addSharedPrior branch
        // below, so the count covers the same population on both fold routes.
        if (ctx.carry.resume_cpl_census_pending)
            ctx.counters.n_resume_cpl_diag_border =
                static_cast<long long>(leftovers.size());
        // Leftovers are the keys the dense fold could not take (no matrix row after a
        // warm resume, or a non-finite screen; card Q3 §2.1).  Under the gauge-free fold
        // they form their own group, and a constellation that contributes one leftover
        // clock is entirely gauge, so nothing is folded for it.  This is conservative by
        // construction: the fallback can under-anchor but cannot re-introduce gauge
        // information.
        // Card NOM N2a: the leftovers are the border carry folded diagonally.
        const ContributionKind leftover_kind = ctx.knobs.carry_gauge_free
                                                   ? ContributionKind::BorderCarryGaugeFree
                                                   : ContributionKind::BorderCarryDiagonal;
        est.contributionKind(leftover_kind);
        if (ctx.knobs.carry_gauge_free)
            ctx.counters.cta_gf_keys +=
                static_cast<long long>(est.addSharedPriorClockGaugeFree(
                    leftovers, ctx.knobs.carry_gauge_free_closure));
        else
            est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, leftovers, leftover_kind);
    }
    // Card PRM phase 3a: a clock-pivot handover in legacy (unscoped) mode skips both fold
    // branches, retiring every carried entry for the epoch; tallied here (counters only, the
    // entries' records are not written).  A held entry keeps its Held tally.
    if (datum_changed && !scoped_handover)
        for (const CarryPrior& cp : prev_carry) {
            const bool held = cp.fold.disposition == CarryDisposition::Held;
            ++ctx.counters.prm_disp[static_cast<int>(cp.kind)][static_cast<int>(
                held ? CarryDisposition::Held : CarryDisposition::Retired)];
        }
    if (prm_reexpressed_epoch) ++ctx.counters.prm_reexpressed_epochs;   // card PRM phase 1b
    // Card CTU G5: the seam's level noise is one epoch's process noise.
    ctx.carry.clm_seam_q.clear();
    // Card SCL: a clone that did not stand in is free when rows reference it and takes an
    // isolated prior when none does (a solve no-op on every other state), whichever branch
    // above ran, including none.  No-op off.
    networkClockCloneFoldEnd(ctx, ws, est, within_epoch_armed);
    // Card SCL2 X2: the registered shared-history columns' N(0, 1) priors, independent of
    // the border (filter/NetworkSharedHistory).  No-op off.
    networkSharedHistoryFold(ctx, ws, est, within_epoch_armed);
    // The partial-support skip tally (card C-GF) is run-scoped on the estimator and is
    // read once at teardown; reading it here would miss the withdraw and
    // within-epoch-replay folds, which run after this point in the epoch.

    // (Fix A hold: the held phase-bias datum is injected before the gate by
    // networkInjectPhaseHoldPrior, next to the SH-VTEC prior; it is a datum, so the
    // gate has to see it.)
}

void networkCouplingExactPrepare(NetworkRunContext& ctx, const NetworkEstimator& est,
                                 const GnssTime& t, double dt,
                                 const std::shared_ptr<spdlog::logger>& log) {
    CouplingExactEpoch& ep = ctx.carry.cxu_epoch;
    ep = CouplingExactEpoch{};
    if (!ctx.knobs.carry_cpl_exact) return;
    // Card SCL: forget the previous epoch's rows' copy (rebuilt at the end once this epoch's
    // inputs are built).  No-op off.
    networkClockClonePrepare(ctx, {});
    ++ctx.counters.cxu_epochs;
    const SharedCarryCov& pcc = ctx.carry.prev_carry_cov;
    if (!ctx.carry.cxu_stash_valid || pcc.P.rows() == 0 || pcc.row.empty()) {
        ++ctx.counters.cxu_epochs_nostash;
        return;
    }
    // --- The rows' view of each key: the fold's own decision (card PRM phase 1b) ------
    // Decided once before PASS B (networkCarryDecide) and read here and by the fold.  A key
    // the fold folds is in S at the variance the decision forms on its stashed variance
    // (the dense fold's chain, the switch-on seam's level noise included); a key the fold
    // does not carry goes to D with its route, and one whose column left the layout as
    // departed.  The one clause no point before PASS B can know, a clock-pivot handover,
    // is applied by the fold, which counts the keys the handover re-expressed after these
    // rows were built.  VtecSh never enters the dense block, and a parked or seeding
    // entry has no dense row.
    const auto rowsVar = [&](const CarryPrior& cp, CouplingExactDRoute& route) -> double {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        switch (cp.fold.disposition) {
            case CarryDisposition::Reref:     // a re-referenced phase-bias frame
                route = CouplingExactDRoute::Reref;
                return nan;
            case CarryDisposition::Reinit:    // decorrelated by the fold: q = infinity
                route = CouplingExactDRoute::Reinit;
                return nan;
            case CarryDisposition::Folded:
            case CarryDisposition::Anchored:
            case CarryDisposition::Loose:
                break;
            default:                          // held, on the oscillator, parked, seeding
                route = CouplingExactDRoute::Skipped;
                return nan;
        }
        const double var_eff = carryFoldVarEff(ctx, cp, cp.fold, cp.var, /*dense=*/true);
        if (!(var_eff > 0.0)) {
            route = CouplingExactDRoute::Skipped;
            return nan;
        }
        route = CouplingExactDRoute::Present;
        return var_eff;
    };
    const auto columnOf = [&](const CarryPrior& cp) -> Eigen::Index {
        try {
            switch (cp.kind) {
                case SharedParamKind::Clock:     return est.clockColumn(cp.sat);
                case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cp.sat, cp.signal);
                case SharedParamKind::CodeBias:  return est.codeBiasColumn(cp.sat, cp.signal);
                default:                         return -1;
            }
        } catch (...) {}
        return -1;
    };
    const int np = static_cast<int>(pcc.P.rows());
    ep.P = pcc.P;
    ep.p_of_key.clear();
    ep.s_of_p.assign(static_cast<std::size_t>(np), -1);
    ep.route_p.assign(static_cast<std::size_t>(np), static_cast<int>(CouplingExactDRoute::Skipped));
    ep.clk_p.assign(static_cast<std::size_t>(np), 0);
    ep.cc_p.assign(static_cast<std::size_t>(np), '\0');
    std::vector<const CarryPrior*> cp_of_p(static_cast<std::size_t>(np), nullptr);
    for (const CarryPrior& cp : ctx.carry.prev_carry) {
        if (cp.kind == SharedParamKind::VtecSh) continue;
        const std::string key = sharedCarryKey(cp.kind, cp.sat, cp.signal);
        const auto rit = pcc.row.find(key);
        if (rit == pcc.row.end() || static_cast<int>(rit->second) >= np) {
            // A leftover (the fold's diagonal route): its inflation is the scalar fallback
            // of the rows' terms on it.
            CouplingExactDRoute r;
            const double v = rowsVar(cp, r);
            if (std::isfinite(v)) ep.q_outside[key] = std::max(0.0, v - cp.var);
            continue;
        }
        cp_of_p[rit->second] = &cp;
    }
    std::vector<double> qs, sh;
    for (int p = 0; p < np; ++p) {
        const CarryPrior* cp = cp_of_p[static_cast<std::size_t>(p)];
        if (!cp) continue;                   // a dense row with no carried entry: D (Skipped)
        const std::string key = sharedCarryKey(cp->kind, cp->sat, cp->signal);
        ep.p_of_key[key] = p;
        ep.clk_p[static_cast<std::size_t>(p)] = (cp->kind == SharedParamKind::Clock) ? 1 : 0;
        // The constellation letter is read for clock rows only (their sat is a satellite).
        ep.cc_p[static_cast<std::size_t>(p)]  =
            cp->sat.empty() ? '\0' : letterOfSat(cp->sat);
        CouplingExactDRoute route;
        const double var_eff = rowsVar(*cp, route);
        if (!std::isfinite(var_eff)) {
            ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(route);
            continue;
        }
        const Eigen::Index col = columnOf(*cp);
        if (col < 0) {
            ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Departed);
            continue;
        }
        double q = var_eff - pcc.P(p, p);    // the fold sets the diagonal of P to var_eff
        // (the seam's level noise is inside var_eff through the decision, like the fold's)
        if (q < 0.0) {
            ++ctx.counters.cxu_q_negative;
            q = 0.0;
        }
        ep.route_p[static_cast<std::size_t>(p)] = static_cast<int>(CouplingExactDRoute::Present);
        ep.s_of_p[static_cast<std::size_t>(p)] = static_cast<int>(ep.p_of_s.size());
        ep.p_of_s.push_back(p);
        ep.col_of_s.push_back(col);
        qs.push_back(q);
        sh.push_back(cp->value);
    }
    for (int p = 0; p < np; ++p) {
        const int r = ep.route_p[static_cast<std::size_t>(p)];
        if (r == static_cast<int>(CouplingExactDRoute::Present)) ++ctx.counters.cxu_keys_s;
        else if (r >= 0 && r < kCouplingExactRoutes) ++ctx.counters.cxu_keys_d[r];
    }
    ep.q_s    = Eigen::Map<const Eigen::VectorXd>(qs.data(), static_cast<Eigen::Index>(qs.size()));
    ep.shat_s = Eigen::Map<const Eigen::VectorXd>(sh.data(), static_cast<Eigen::Index>(sh.size()));
    ep.stash_s = ctx.carry.cxu_stash_s;
    // Card CTU G5: the off-diagonal half of a switch-on seam's level noise (the fold adds
    // the same between the satellite's clock and phase-bias keys), from the decision's seam
    // noise (a clock or phase bias of a seam satellite that is not a reinit).
    ep.q_off.clear();
    if (!ctx.carry.clm_seam_q.empty()) {
        const Eigen::Index nS = ep.nS();
        const auto seamCp = [&](Eigen::Index a) -> const CarryPrior* {
            const CarryPrior* c = cp_of_p[static_cast<std::size_t>(ep.p_of_s[static_cast<std::size_t>(a)])];
            return (c && c->fold.has_seam) ? c : nullptr;
        };
        for (Eigen::Index a = 0; a < nS; ++a) {
            const CarryPrior* ca = seamCp(a);
            if (!ca) continue;
            for (Eigen::Index b = 0; b < nS; ++b) {
                if (b == a) continue;
                const CarryPrior* cb2 = seamCp(b);
                if (cb2 && cb2->sat == ca->sat)
                    ep.q_off.emplace_back(static_cast<int>(a), static_cast<int>(b), ca->fold.seam_q_m2);
            }
        }
    }
    // Card PFQ: the pairwise half of the pivot-frame clock noise, the same as the dense fold
    // adds (its diagonal half is inside q_s through the decision), so the rows are
    // re-expressed with the fold's own Q_S.
    if (ctx.knobs.pfq_on) {
        const Eigen::Index nS = ep.nS();
        const auto pfqCp = [&](Eigen::Index a) -> const CarryPrior* {
            const CarryPrior* c = cp_of_p[static_cast<std::size_t>(ep.p_of_s[static_cast<std::size_t>(a)])];
            return (c && c->kind == SharedParamKind::Clock && c->fold.pfq_rate_m2ps > 0.0
                    && !c->sat.empty()) ? c : nullptr;
        };
        for (Eigen::Index a = 0; a < nS; ++a) {
            const CarryPrior* ca = pfqCp(a);
            if (!ca) continue;
            for (Eigen::Index b = 0; b < nS; ++b) {
                if (b == a) continue;
                const CarryPrior* cb2 = pfqCp(b);
                if (!cb2 || systemOfSat(cb2->sat) != systemOfSat(ca->sat)) continue;
                ep.q_off.emplace_back(static_cast<int>(a), static_cast<int>(b),
                                      pivotFrameCross(ca->fold.pfq_rate_m2ps, ca->fold.dt_applied_s,
                                                      cb2->fold.dt_applied_s));
                ++ctx.counters.pfq.cxu_pairs;
            }
        }
    }
    // The pivots, re-resolved in this epoch's layout.  A pivot that left the layout keeps
    // no entry, and the rows count the deficit as an orphan.
    bool any_clk = false;
    for (int p = 0; p < np; ++p) any_clk = any_clk || ep.clk_p[static_cast<std::size_t>(p)];
    for (const auto& [cc, pv] : ctx.carry.cxu_pivot) {
        try {
            const Eigen::Index col = est.clockColumn(pv.first);
            if (col >= 0) ep.pivot[cc] = {col, pv.second};
        } catch (...) {}
    }
    if (any_clk && ep.pivot.empty()) ++ctx.counters.cxu_pivot_missing;
    if (!couplingExactFactor(ep)) {
        ++ctx.counters.cxu_epochs_unfactored;
        log->warn("Network (card CTU): P_SS + Q_S over {} carried key(s) did not factor at "
                  "TOW {:.1f}; the coupling rows take the card 1d route this epoch.",
                  ep.nS(), t.tow);
        return;
    }
    ++ctx.counters.cxu_epochs_built;
    // Card SCL: the rows' copy of this epoch with each cloned clock on its clone at Q = 0
    // (filter/NetworkClockClone).  This epoch itself stays as built: card SMH and the fold's
    // counters read it.  No-op off.
    networkClockClonePrepare(ctx, cp_of_p);
}

void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                NetworkRunTraces& traces, const GnssTime& t,
                                NetworkEstimator& est,
                                const std::vector<std::string>& registered_clock_sats,
                                bool clk_model_warm, bool datum_changed,
                                const std::set<char>& changed_consts,
                                double pl_now_s, double dt, bool within_epoch_armed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& clock_osc = ctx.carry.clock_osc;
    const double clk_model_q1 = ctx.knobs.clk_model_q1;
    const double clk_model_q2 = ctx.knobs.clk_model_q2;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;
    const bool   scoped_handover = ctx.knobs.scoped_handover;
    auto& clk_pred = ws.clk_pred;
    auto& cap_priors = ws.cap_priors;
    auto& clk_prior_info = ws.clk_prior_info;
    auto& anchor_info_trace = traces.anchor_info_trace;
    // Scope predicate (RTA-2), re-declared over the explicit (datum_changed,
    // changed_consts) parameters, with the same read points.
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };

    // --- Two-state oscillator clock carry-forward prior (network.clock_model) -----------
    // The satellite clock's prior comes from the physical clock model: predict
    // [offset, drift] forward and fold the offset (mean = drift-following prediction,
    // info = 1/predicted var).  Because the prediction is tight (cm level), the
    // post-solve innovation exposes a lock-in that the scalar random walk would freeze.
    // Skipped on a pivot handover (retired datum) and for the first warm epoch of each
    // satellite (no prediction until it is seeded).  Under
    // network.scoped_handover_response (RTA-2) a handover retires only the changed
    // constellations' datum, so only their satellites skip the prediction (they re-seed
    // from this epoch's new-datum solve in the post-solve update); oscillators of
    // unaffected constellations predict normally.  Off is byte-identical (the outer
    // gate reduces to !datum_changed, and the per-satellite skip cannot trigger inside
    // it).
    if (clk_model_warm && (!datum_changed || scoped_handover)) {
        std::vector<NetworkEstimator::SharedPrior> clkp;
        clkp.reserve(clock_osc.size());
        for (const std::string& sat : registered_clock_sats) {
            if (handover_affects(sat)) continue;  // retired-datum satellite, scoped mode only (RTA-2)
            auto it = clock_osc.find(sat);
            if (it == clock_osc.end() || !it->second.init) continue;
            // Predict over this satellite's true elapsed time, not the global epoch dt
            // (P0-3).  A satellite absent for several epochs (eclipse exclusion is on in
            // the clock-model config) would otherwise get an hours-old state predicted
            // over one dt of Q, a falsely tight prior.  Past the gap bound its carried
            // state is stale: the prior is dropped (reseed), so the clock re-acquires from
            // data and the a-priori anchor instead of an old value.  (Within the bound,
            // the Q·dt inflation over the true gap de-weights the prior, which limits
            // itself.)
            const double sat_dt = (it->second.t_last > 0.0)
                                      ? (pl_now_s - it->second.t_last) : std::max(0.0, dt);
            if (!(sat_dt >= 0.0) || sat_dt > clk_model_max_gap) {
                // Event for the stale-carry reseed, which is otherwise not reported (T5,
                // V2 blind spot).  It is edge-triggered by construction: the update
                // reseeds t_last, so the next epoch is fresh.
                traces.emitEvent(t, "T5_clock_model", "stale_prior_reseed", sat, "", sat_dt,
                          clk_model_max_gap, "reseed", "carry past gap");
                continue;   // stale → no prior, reseed (R11)
            }
            const ClockOscPred pr = clockOscPredict(it->second, sat_dt,
                                                    clk_model_q1, clk_model_q2);
            if (!(pr.p00 > 0.0) || !std::isfinite(pr.x0)) continue;
            clk_pred[sat] = pr;
            NetworkEstimator::SharedPrior p;
            p.kind = SharedParamKind::Clock; p.sat = sat; p.signal = "";
            p.value = pr.x0; p.info = 1.0 / pr.p00;
            if (anchor_info_trace.is_open()) clk_prior_info[sat] = p.info;  // two-state clock-model prior info (B7)
            clkp.push_back(p);
        }
        est.contributionKind(ContributionKind::ClockOscillator);   // card NOM N2a
        est.addSharedPrior(clkp);
        if (within_epoch_armed)
            networkCapturePriors(cap_priors, ws.cap_prior_kinds, clkp,
                                 ContributionKind::ClockOscillator);
    }
}

void networkRxCbDemeanPrepass(NetworkRunContext& ctx,
                              const std::vector<NetStation>& stations,
                              const std::vector<bool>& present,
                              double pl_now_s) {
    if (!ctx.knobs.rx_cb_on || !ctx.knobs.rx_cb_demean
        || !ctx.knobs.rx_cb_demean_within) return;
    const auto& rx_cb_carry = ctx.carry.rx_cb_carry;   // read-only here
    // Clear the per-epoch trace record first: an epoch that computes no μ (cold start,
    // or a full carry wipe) must not leave the previous epoch's row for the trace
    // emitter to publish under this epoch's timestamp.
    ctx.carry.rx_cb_demean_last.clear();
    if (rx_cb_carry.empty()) return;                   // cold start: nothing to average
    // The horizon is the inject's own, kRxCbMaxGapS (filter/NetworkRunTypes.hpp).

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
            if (cbit == rx_cb_carry.end()) continue;           // cold: nothing to fold
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

    // Same reduction, thin-mean floor and freeze semantics as the LV-2 harvest path;
    // only the epoch the result is applied to differs.  Writing
    // ctx.carry.rx_cb_demean_mu here (serially, before PASS B) removes the one-epoch
    // lag, and the parallel produce phase reads it as before.
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
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    auto& iono_carry = ctx.carry.iono_carry;   // slant-iono carry (queue 8b)
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
    const bool   pl_los_scale = ctx.knobs.pl_los_scale;   // mirror the injector's per-LOS B_eff in the fold-variance readback (F1)
    const bool   pl_local_block = ctx.knobs.pl_local_block; // stash the harvest cross-covariance block beside the per-column carry (F3)
    // Birth-prior floor that the stash records in LocalCarry::var_birth (RF-1 §3.3(b)).
    // 0.0 (default) ⇒ var_birth == var_first to the ULP, so the floor has no effect.
    const double amb_birth_var_floor = ctx.knobs.amb_birth_var_floor;
    // The σ-power of the birth floor's elevation mapping is the code weights' power,
    // because the floor states what a code-determined level is worth (RF-1 step 4,
    // design §3.3(b)).
    const double amb_elev_power = config_.getCodeElevationPower();
    // Per-arc carry-state trace (network.trace_amb_state, RF-1 §7.4).  The stride gates
    // trajectory rows only; an event (birth / drop / erase) is written on every epoch,
    // so the trace is a complete inventory of events.  Off, the whole block below is
    // unreachable (byte-identical).
    const bool amb_state_on = ctx.knobs.trace_amb_state;
    const bool amb_state_tr =
        amb_state_on
        && (ctx.knobs.trace_amb_state_every <= 1
            || (static_cast<long long>(ctx.counters.n_loop_epochs)
                % ctx.knobs.trace_amb_state_every) == 0);
    // Age in epochs for the trace's relevel clock uses the run's own observed
    // inter-epoch spacing, as the young-arc inflation derives it, so the column means the
    // same thing at 1 s and 30 s cadence without a nominal rate.  It is read here, not
    // later: ctx.carry.pl_prev_now_s is advanced to this epoch at the end of this
    // function, so inside the stash loop it still holds the previous stamp.
    const double amb_state_dt_ep_s = [&] {
        const double dt = pl_now_s - ctx.carry.pl_prev_now_s;
        return (std::isfinite(dt) && dt > 0.0 && dt <= pl_max_gap_s) ? dt : 0.0;
    }();
    const bool   zwd_on = ctx.knobs.zwd_on;
    const bool   iono_on = ctx.knobs.iono_on;             // slant-iono carry (queue 8b)
    const double iono_q = ctx.knobs.iono_q;
    const double iono_var_floor = ctx.knobs.iono_var_floor;
    const double iono_max_gap_s = ctx.knobs.iono_max_gap_s;
    const double eq34_rpb_q = ctx.knobs.eq34_rpb_q;
    const bool   phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // Prior-residual census: the pl / zwd / rxcb feed arms.  Resolved once at run start
    // into per-family bools, so an off run pays one bool test per harvest loop and
    // nothing else.
    const bool   prires_pl   = ctx.knobs.prires_pl;
    const bool   prires_zwd  = ctx.knobs.prires_zwd;
    const bool   prires_iono = ctx.knobs.prires_iono;   // slant-iono carry (queue 8b)
    const bool   prires_rxcb = ctx.knobs.prires_rxcb;
    // There is no `zwd_q` alias here, on purpose (RS-10b, §21.21): this body's one
    // consumer (the prior-residual census's fold_var) reads the effective q through
    // NetworkKnobs::zwdQEff(zwd_seam_live), because inside a live seam window an armed
    // network.zwd_seam_reseed_q replaces network.zwd_carry_q at the injector.  A bare
    // zwd_q would mis-scale the `zwd` family's σ̂₀ without any warning.
    // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): the ZWD column
    // joins the F3 harvest block.  It is gated on the same live predicate as the block
    // itself (pl_local_block && carry_cpl_cond), so the harvest and the inject agree on
    // which station-epochs have a block; carry_cpl_joint is the injector's mutex and
    // does not reach the block path there either.  Off ⇒ `cols` is untouched and
    // PlHarvestBlock::C keeps its dimension without the ZWD column (byte-identical).
    const bool   zwd_blk_on  = ctx.knobs.zwd_in_block && pl_local_block && carry_cpl_cond;
    // Iono in the local block (network.carry_iono_in_block): the per-LOS iono columns
    // join the same harvest block, on the same live predicate, for the same reason.
    // Off ⇒ `cols` is untouched and PlHarvestBlock::C keeps its dimension
    // (byte-identical).
    const bool   iono_blk_on = ctx.knobs.iono_blk && pl_local_block && carry_cpl_cond;
    // Receiver clock in the local block (network.carry_rx_clock_in_block): the
    // receiver-clock (and ISB) columns join the same harvest block, on the same live
    // predicate, for the same reason.  Off ⇒ `cols` is untouched and PlHarvestBlock::C
    // keeps its dimension (byte-identical).
    const bool   rxclk_blk_on  = ctx.knobs.rxclk_blk && pl_local_block && carry_cpl_cond;
    const double rxclk_gap_s   = ctx.knobs.rxclk_max_gap_s;
    const double rxclk_q       = ctx.knobs.rxclk_q_m2;
    const double rxclk_rate_q  = ctx.knobs.rxclk_rate_q;
    const double rxclk_rate_var0 = ctx.knobs.rxclk_rate_var0;
    const double rxclk_jump_k  = ctx.knobs.rxclk_jump_k;
    // RXCB-IN-BLOCK (network.carry_rx_code_bias_in_block, card CTU section 14): the
    // receiver code-bias columns join the same harvest block and are stashed with their
    // border regression and block row here; the standalone diagonal harvest below then
    // skips its stash.  Off => `cols` is untouched (byte-identical).
    const bool   rxcb_blk_on   = ctx.knobs.rxcb_blk && ctx.knobs.rx_cb_on
                                 && pl_local_block && carry_cpl_cond;
    // Seam-scoped ZWD re-seed + unconditional Z-row capture (§17 finding 3).
    const bool   zwd_capture   = ctx.knobs.zwd_capture;
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;

    // --- Two-edge continuous handover: ledger harvest --------------------------
    //
    // Record, for every anchor-candidate column, the station's absolute ambiguity
    // estimate in this epoch's realized frame, so that if the anchor for that key moves
    // next epoch the election can pin the successor at this value instead of at 0,
    // which makes the gauge change an S-transform rather than a re-realization.
    //
    // The election cannot read `st.sm->getStateVector()(amb_idx)` instead: that is
    // `x_lin`, a linearization frame.  The network solve is never written back into the
    // per-station StateManager (`setStateVector` runs once, at setup), and an `x_lin`
    // shift on a local column is an exact solve no-op, so pinning to it would carry no
    // information about where the arc sits.  The estimate is `x_lin + back-substituted
    // Δx`, which exists only here, after the solve.  Hence the one-epoch deferral:
    // harvested now, consumed by the next epoch's election, in the same way as
    // carry_innov_pending / dw_release_pending / raim_refloat_pending.
    //
    // The population is `R.qc_amb`, the carry-neutral census: columns present in A_r
    // and not pinned.  A station holding the pin is not a handover target, and its
    // pinned column carries no independent information.  Not gated on carry_warm, on
    // purpose: the ledger has to work on lineages where the phase-leveling carry does
    // not exist.
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
                // The pin is a hard constraint (weight ~1e10), so the re-coupling edge
                // has to re-validate the value before applying it (invariant #7).  This
                // uses the border-exact variance, not the leveling carry's
                // border-inclusive harvest: the shared phase-bias column's uncertainty
                // moves the pin target and the realized frame together (common mode in
                // the handover step), so including it gates the wrong quantity in both
                // directions, refusing settled successors while passing a large real
                // error (§5.0's G21 case).  The ceiling has to bound the successor's
                // error relative to the shared frame, which is what this returns.
                vhv = est.pureConditionalLocalVarianceDiag(est_idx_hv, cols_hv);
            } catch (...) { continue; }
            for (std::size_t a = 0; a < R.qc_amb.size(); ++a) {
                const PlAmb& pa = R.qc_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                const double var = (a < static_cast<std::size_t>(vhv.size()))
                                       ? vhv(static_cast<Eigen::Index>(a)) : -1.0;
                // Key on "sat_signal", byte-identical to the election's `amb`
                // (kAmbiguityDelimiter is '_'), so the lookup needs no translation.
                AnchorHandoverLedger::Entry hv_e{value_abs, var};
                // network.phase_datum_handover_max_var_ratio: the arc's first-stash
                // conditional variance (0 = never carried ⇒ the ratio test refuses it),
                // read from the previous epoch's carry (this harvest precedes the leveling
                // harvest).
                {
                    const auto lcit = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    if (lcit != local_carry.end()) hv_e.var_first = lcit->second.var_first;
                }
                ctx.carry.handover.value_abs[{static_cast<int>(i),
                                              StateKeys::makeAmbiguityKey(pa.sat, pa.signal)}] = hv_e;
            }
        }
        ctx.carry.handover.primed = true;
    }

    // --- Phase-leveling carry: harvest posterior and stash (increment 1) ----
    // For each present non-pivot station, recover its ambiguities' absolute values
    // (x_lin + back-substituted Δx) and their conditional variances (the cheap
    // per-column diagonal harvest, not the full per-station inverse), and refresh the
    // persistent carry.  Then drop entries unseen beyond the max gap (a stale-carry and
    // map-growth guard, distinct from the slip-driven refloat/drop of later increments).
    if (carry_warm) {
        // Build, once per epoch, the shared column → identity descriptor map and the
        // datum-frame marginal-variance diagonal, so each carried local's regression on
        // the free shared border can be stashed in a layout-stable (identity-keyed) form.
        // Empty and unused on the legacy diagonal path → byte-identical.
        std::map<Eigen::Index, CoupledShared> col2desc;
        Eigen::MatrixXd cov_df;   // shared covariance, datum-pinned + ensemble-gauge cols zeroed
        // Card SCL: the clones' posterior regression on the rest of the border, once per
        // epoch (filter/NetworkClockClone).  Inert off and when no clone is registered.
        ClockCloneHarvest ccl_h;
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
            // P_prev,jj, with which a departed shared column is marginalized next epoch:
            // the datum-frame marginal variance (datum-pinned and ensemble-gauge columns
            // exactly 0, mirroring conditionalLocalVarianceDiag's frame).
            cov_df = est.sharedCovariance();
            for (const auto& p : plan.shared_datum)
                if (p.column >= 0 && p.column < cov_df.rows()) {
                    cov_df.row(p.column).setZero(); cov_df.col(p.column).setZero();
                }
            est.zeroPhaseGaugeColumns(cov_df);
            ccl_h = networkClockCloneHarvestBegin(ctx, est, plan.shared_datum, col2desc);
        }
        // Card SCL2 X2: where each harvested row's entry is written, for the compression at
        // the end (filter/NetworkSharedHistory).  Inert off.
        SharedHistoryHarvest shx_h = networkSharedHistoryHarvestBegin(ctx, ccl_h);
        // Harvest one carried column's pure conditional variance (N_rr⁻¹)_cc and its
        // regression on the free shared border (identity-keyed, sparsified).
        // Returns {cond_var, coupling}; cond_var ≤ 0 on failure → the caller skips.
        // With F3 (network.phase_leveling_local_block), non-null `cross_cols` / `cross`
        // make the same per-column solve also read the station's other carried columns
        // and return them as (N_rr⁻¹)_{c c_j} (a gather, no second solve); both are null
        // on every other call (byte-identical).
        auto harvestCoupling = [&](std::size_t est_idx, Eigen::Index col,
                                   const std::vector<Eigen::Index>* cross_cols,
                                   Eigen::VectorXd* cross)
                -> std::pair<double, std::vector<CoupledShared>> {
            NetworkNormalEquations::StationRegression reg;
            try { reg = est.stationRegression(est_idx, col, /*carry_aware=*/false, cross_cols); }
            catch (...) { return {-1.0, {}}; }
            if (cross) *cross = std::move(reg.cross);
            // Card SCL: the row's coefficients on the clock clones regressed onto the rest of
            // the border and the clones' conditional variance added (the clone columns are
            // not in col2desc, so without this the loop below would drop them).  No-op off.
            networkClockCloneHarvestRow(ccl_h, ctx, est_idx, col, reg.row, reg.cond_var);
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
                    // A datum-pinned or ensemble-gauge column is kept in the row
                    // (coefficient and its ŝ_prev RHS term) with p_prev = 0 ("exactly
                    // known"; the zeroed cov_df diagonal mirrors
                    // conditionalLocalVarianceDiag's frame).  The full regression row is
                    // exactly gauge-invariant: every station data row satisfies
                    // A_s·g_s + A_r·g_r = 0 for each structural gauge direction, so
                    // M = e_cᵀN_rr⁻¹N_rs has Mᵀg_s = −g_r(c), and the row's +1 local
                    // completes the cancellation.  Dropping the pinned columns'
                    // coefficients would leave M'ᵀg ≠ 0: every coupling row would then
                    // observe the clock gauge, and the W5 null-space gate would see "0 null
                    // directions vs declared dim" and withhold every epoch that carries
                    // rows, which leaves the coupling carry inert while the A/B still
                    // reads byte-identical.  Under the datum the pinned term is exact
                    // (Dirichlet elimination), and p_prev = 0 degrades every other path
                    // correctly: the κ time update falls through unshrunk (a pinned column
                    // does not move), and a departed pinned column marginalizes to +0.
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
        // ---- Lane observatory (rung 3 v3): pre-event snapshot ---------------
        // Taken here because the loop below overwrites local_carry entry by entry, so a
        // band's pre-event level is gone once its own iteration runs.  Only the freed
        // population is snapshotted, so the off path and an epoch without slips cost
        // nothing.
        //
        // C(t-) and C(t+) are both read inside this function, so both are in the gauge
        // this epoch's datum work already settled; the frame hazard of the deleted
        // seed-site snap cannot arise here.
        // (value_abs, last_seen_s): the stamp matters, because a carry that was not
        // stashed at the immediately preceding epoch spans a different interval from
        // the one-epoch ΔMW it would be differenced against.
        std::map<std::tuple<int, std::string, std::string>, std::pair<double, double>> lane_pre;
        // Not gated on a non-empty lane_mw: an epoch with freed arcs but no raw rows
        // still has to reach the measurement loop, so each freed LOS is counted in
        // n_lane_skip_no_row; otherwise the denominator would miss those arcs, which the
        // no_row channel exists to prevent.
        const bool lane_obs_on = config_.networkCycleSlipRelevelLaneSnap();
        if (lane_obs_on) {
            for (const auto& fa : ctx.carry.relevel.free_ambs) {
                const auto parts = StateKeys::parseAmbiguityKey(fa.second);
                if (!parts) continue;
                const auto key = std::make_tuple(fa.first, parts->satellite, parts->signal);
                const auto cit = local_carry.find(key);
                if (cit != local_carry.end())
                    lane_pre[key] = { cit->second.value_abs, cit->second.last_seen_s };
                // A freed band's partner may not itself be freed (band-scoped response):
                // snapshot the whole pair, because the partner's carry anchors the lane
                // and the same loop overwrites it.
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

        // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): the ZWD
        // harvest is a separate loop further down (it has to run for stations with no
        // carried ambiguities), so the block this loop allocates, and the `cols` order its
        // rows are indexed by, are handed forward per station.  Empty vectors when the
        // knob is off, and nothing below touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> zblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       zblk_cols_of_sta;
        if (zwd_blk_on) {
            zblk_of_sta.resize(stations.size());
            zblk_cols_of_sta.resize(stations.size());
        }
        // Iono in the local block: the same hand-forward for the per-LOS iono harvest
        // loop below.  `iblk_row0_of_sta` is the block index of this station's first iono
        // column; the iono columns are appended right after the ambiguities and before
        // the ZWD, so the ZWD keeps the last-row slot that its own (validated) card
        // computes as C.rows()−1.  Empty vectors when the knob is off; nothing below
        // touches them.
        std::vector<std::shared_ptr<PlHarvestBlock>> iblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       iblk_cols_of_sta;
        std::vector<Eigen::Index>                    iblk_row0_of_sta;
        if (iono_blk_on) {
            iblk_of_sta.resize(stations.size());
            iblk_cols_of_sta.resize(stations.size());
            iblk_row0_of_sta.assign(stations.size(), -1);
        }
        // Receiver clock in the local block: the same hand-forward for the
        // receiver-clock harvest loop.  `rblk_row0_of_sta` is the block index of this
        // station's first clock-family column, appended after the iono columns and before
        // the ZWD, so the ZWD keeps the last-row slot that its own (validated) card
        // computes as C.rows()−1.
        std::vector<std::shared_ptr<PlHarvestBlock>> rblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       rblk_cols_of_sta;
        std::vector<Eigen::Index>                    rblk_row0_of_sta;
        if (rxclk_blk_on) {
            rblk_of_sta.resize(stations.size());
            rblk_cols_of_sta.resize(stations.size());
            rblk_row0_of_sta.assign(stations.size(), -1);
        }
        // RXCB-IN-BLOCK: the same hand-forward for the receiver code-bias loop below.
        std::vector<std::shared_ptr<PlHarvestBlock>> cblk_of_sta;
        std::vector<std::vector<Eigen::Index>>       cblk_cols_of_sta;
        std::vector<Eigen::Index>                    cblk_row0_of_sta;
        if (rxcb_blk_on) {
            cblk_of_sta.resize(stations.size());
            cblk_cols_of_sta.resize(stations.size());
            cblk_row0_of_sta.assign(stations.size(), -1);
        }
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            // Prioritized datum: harvest the former pivot too.  Its non-anchored arcs are
            // in R.pl_amb, and anchored (pinned) keys were excluded at build, so inject
            // and harvest partition on the same per-(sat,signal) predicate.
            if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
            const StationAssembly& R = asm_res[i];
            // Iono in the local block: a station with no carry-eligible ambiguities can
            // still have live per-LOS iono columns, and PASS B builds a block for that
            // case (its `if (!pl_pend.empty() || !iono_bc.empty())` gate).  Skipping it
            // here would stash those entries without a block, so every pair with them
            // would be zeroed next epoch, giving a diagonal iono prior (the vehicle the
            // in-block carry replaces).  Harvest and inject have to agree on which
            // station-epochs have a block.  Off ⇒ iono_blk_on is false and this reduces
            // to `if (R.pl_amb.empty()) continue;` (byte-identical).
            // Receiver clock in the local block: the same holds for a station whose only
            // live carry is its receiver clock.  PASS B builds a block for that case too,
            // and skipping it here would stash the entry without a block, diagonalizing
            // the most station-common direction of all.
            if (R.pl_amb.empty() && !(iono_blk_on && !R.pl_iono.empty())
                && !(rxclk_blk_on && !R.pl_rxclk.empty())
                && !(rxcb_blk_on && !R.pl_cb.empty())
                && !(ctx.knobs.pc_on && !R.pl_pc.empty())) continue;
            const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
            std::vector<Eigen::Index> cols;
            cols.reserve(R.pl_amb.size() + 1 + R.pl_iono.size() + R.pl_rxclk.size());
            for (const auto& a : R.pl_amb) cols.push_back(a.col);
            // Iono in the local block: the station's present per-LOS iono columns become
            // block indices n_amb … n_amb+k−1, appended before the ZWD so the ZWD keeps
            // its invariant that the appended column is the last row.  Every per-column
            // solve below then gathers (N_rr⁻¹)_{c_a, icol} at no extra cost, the same
            // gather that already covers the ambiguity–ambiguity terms.  R.pl_iono is
            // PASS B's record of what it pushed, so a station whose block was never built
            // keeps `cols` as it was.
            const Eigen::Index iono_row0 = static_cast<Eigen::Index>(cols.size());
            const bool iono_in_cols = iono_blk_on && !R.pl_iono.empty();
            if (iono_in_cols)
                for (const auto& t : R.pl_iono) cols.push_back(std::get<1>(t));
            // ZWD in the local block (B9-in-block): the station's ZWD column becomes block
            // index n (the last entry), so every per-column solve below gathers its
            // (N_rr⁻¹)_{c_a, zcol} cross term at no extra cost, the same gather that
            // already covers the ambiguity-ambiguity terms.  It is appended after the
            // ambiguity columns so the pl rows keep their indices `a`; the ZWD loop below
            // writes the ZWD's own row n.  R.pl_zwd_present is PASS B's record, so a
            // station whose tropo column was absent this epoch keeps `cols` without it.
            // Receiver clock in the local block: the station's receiver-clock (and ISB)
            // columns become the next block indices, appended after the iono columns and
            // before the ZWD so the ZWD keeps its last-row invariant.  Every per-column
            // solve below then gathers (N_rr⁻¹)_{c_a, rcol} at no extra cost.
            const Eigen::Index rxclk_row0 = static_cast<Eigen::Index>(cols.size());
            const bool rxclk_in_cols = rxclk_blk_on && !R.pl_rxclk.empty();
            if (rxclk_in_cols)
                for (const auto& t : R.pl_rxclk) cols.push_back(t.col);
            // RXCB-IN-BLOCK: every present receiver code-bias column, carried or cold, so
            // each is stashed with its cross terms to the station's other columns.
            const Eigen::Index rxcb_row0 = static_cast<Eigen::Index>(cols.size());
            const bool rxcb_in_cols = rxcb_blk_on && !R.pl_cb.empty();
            if (rxcb_in_cols)
                for (const auto& t : R.pl_cb) cols.push_back(std::get<1>(t));
            // Card LGM: the station's present phase-colour columns, after the receiver
            // code bias and before the ZWD (which keeps its last-row slot).
            const Eigen::Index pc_row0 = static_cast<Eigen::Index>(cols.size());
            const bool pc_in_cols = ctx.knobs.pc_on && !R.pl_pc.empty();
            if (pc_in_cols)
                for (const PlPc& pp : R.pl_pc) cols.push_back(pp.col);
            const bool zwd_in_cols = zwd_blk_on && R.pl_zwd_present
                                     && R.pl_zwd.col >= 0;
            if (zwd_in_cols) cols.push_back(R.pl_zwd.col);
            Eigen::VectorXd xr, vard;
            try {
                xr = est.reconstructStation(est_idx, x_s);
                if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc, not this marginal
                    vard = est.conditionalLocalVarianceDiag(est_idx, cols, plan.shared_datum);
            } catch (...) { continue; }
            // Local block (F3, network.phase_leveling_local_block): this station-epoch's
            // harvest cross-covariance block C(a,b) = (N_rr⁻¹)_{c_a c_b} over R.pl_amb's
            // columns.  One shared_ptr per station-epoch, filled row by row below from
            // each column's own n_c solve (the gather stationRegression already performs),
            // and referenced by every entry stashed at this harvest.  It is re-stashed
            // whole every epoch from the posterior, so Σ does not drift across epochs; the
            // previous block is released when its last referencing entry is replaced.
            // Null when the knob is off ⇒ nothing below allocates or writes.
            std::shared_ptr<PlHarvestBlock> hblk;
            if (pl_local_block && carry_cpl_cond) {
                hblk = std::make_shared<PlHarvestBlock>();
                const Eigen::Index nb = static_cast<Eigen::Index>(cols.size());
                hblk->C = Eigen::MatrixXd::Constant(nb, nb, std::numeric_limits<double>::quiet_NaN());
                hblk->stamp_s = pl_now_s;
                // ZWD in the block: hand the block and its column order to the ZWD loop
                // below, which owns row n.  Handed over only when the ZWD is in `cols`;
                // otherwise the ZWD loop would write a row that does not exist.
                if (zwd_in_cols) {
                    zblk_of_sta[i]      = hblk;
                    zblk_cols_of_sta[i] = cols;
                }
                // Iono in the block: same hand-forward, plus the row offset the iono loop needs.
                if (iono_in_cols) {
                    iblk_of_sta[i]      = hblk;
                    iblk_cols_of_sta[i] = cols;
                    iblk_row0_of_sta[i] = iono_row0;
                }
                // Receiver clock in the block: same hand-forward, plus its loop's row offset.
                if (rxclk_in_cols) {
                    rblk_of_sta[i]      = hblk;
                    rblk_cols_of_sta[i] = cols;
                    rblk_row0_of_sta[i] = rxclk_row0;
                }
                if (rxcb_in_cols) {   // RXCB-IN-BLOCK
                    cblk_of_sta[i]      = hblk;
                    cblk_cols_of_sta[i] = cols;
                    cblk_row0_of_sta[i] = rxcb_row0;
                }
                // Card SCL: the block takes the within-station half of the clones'
                // conditional covariance once every column is harvested.  No-op off.
                networkClockCloneHarvestBlock(ccl_h, est_idx, hblk, cols);
            }
            for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                const PlAmb& pa = R.pl_amb[a];
                if (pa.col < 0 || pa.col >= xr.size()) continue;
                const double value_abs = pa.x_lin + xr(pa.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                Eigen::VectorXd cross;   // (N_rr⁻¹)_{c_a c_b} over the station's carried columns, empty when off (F3)
                if (carry_cpl_cond) {
                    auto hc = harvestCoupling(est_idx, pa.col, hblk ? &cols : nullptr,
                                              hblk ? &cross : nullptr);
                    var = hc.first; coupling = std::move(hc.second);
                    // Coupling observability (cards 1 and 1d): a failed harvest drops the
                    // carry entirely; an empty regression degrades the row to a plain
                    // diagonal, which only this counter shows.
                    if (!(var > 0.0) || !std::isfinite(var)) ++n_cpl_harvest_fail;
                    else if (coupling.empty())               ++n_cpl_stash_empty;
                    else                                     ++n_cpl_stash;
                } else {
                    var = (a < static_cast<size_t>(vard.size())) ? vard(a) : -1.0;
                }
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                // LS-VCE monitor (M2): fold this ambiguity carry's post-fit innovation
                // (carried prior − freshly solved) into the monitor before the stash
                // overwrites the prior.  Only rows that carried a prior this epoch
                // qualify, mirroring the inject predicate: a live prior within max-gap,
                // not a slip or refloat (pending reseed), and not a card-3 re-referenced
                // arc (a frame step, not noise).
                // Prior-residual census: the same (prior − solved, gap, post_var,
                // fold_var) tuple the VCE monitor folds, but without its two exclusions,
                // which become the RESPONDED / FRAME labels; that population is what a
                // detector exists to find and what a variance component should drop.
                // Hoisted above the monitor so both read one qualification.
                //
                // `folded` is reproduced from the inject predicate, not mirrored by hand:
                // every clause of the PASS-B injector is a pure function of values still
                // in scope here (the carry entry, the gap, the pending-reseed mark, which
                // is cleared only after this point, pa.x_lin and var_eff).  An error here
                // matters: the folded branch on an unfolded row inflates |t| about 3000×
                // through the ε clamp and would make the positive control pass
                // vacuously.  Two clauses cannot be reproduced here and are assumed to
                // hold, since the ambiguity reached R.pl_amb: the gauge-pin exclusion and
                // the h2local column lookup.  eq34_reref is a frame label for pl, not a
                // folded=false condition: the pl injector has no eq34 skip, so a
                // re-referenced arc did fold.
                const bool prires_want = prires_pl && prires_est.enabled();
                if (carry_vce || prires_want) {
                    const auto oc = local_carry.find(
                        std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                    const bool reseed_pending =
                        stations[i].sm->isAmbiguityPendingReseed(pa.amb_key);
                    const bool reref = eq34_reref.count({pa.sat, pa.signal}) > 0;
                    // Harvest guard (SPEC_slip_relevel_walk): a freed or bridged arc's
                    // (prior − solved) innovation is slip-scale, and without this guard
                    // it would be ingested as a clean row (the PASS-B pre-pass consumed
                    // the detector's own mark, so reseed_pending is false here for every
                    // slip the detector caught).
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
                                    // Mirror of the F1 scaling: the injector multiplied this
                                    // column's variance by its LOS B_eff (before the floor).  Keyed
                                    // by the injected (sat, signal), so a sibling band the
                                    // inject skipped (freed / stale / reseed) is not scaled
                                    // here either: the mirror reports the weight the fold
                                    // used, or the hand value when it did not fold.
                                    // Under F3 (phase_leveling_local_block) a column carried
                                    // in a whitened block folded with the block's joint Σ,
                                    // whose diagonal for this column is this hand value
                                    // (plus the card-1d / departed terms neither path
                                    // mirrors), so the mirror reports Σ's diagonal, the
                                    // column's marginal prior variance, unscaled.  This is
                                    // deliberate: the census standardizes e by the marginal
                                    // prior σ the column carried, and pl_los_beff is filled
                                    // only on the F1 route (under F3, a factorization
                                    // fallback), so the ×B_eff below is absent for block
                                    // columns.
                                    if (pl_los_scale) {
                                        const auto bf = R.pl_los_beff.find({pa.sat, pa.signal});
                                        if (bf != R.pl_los_beff.end() && bf->second > 1)
                                            v *= static_cast<double>(bf->second);
                                    }
                                    if (pl_var_floor > 0.0) v = std::max(v, pl_var_floor);
                                    return v;
                                }();
                            // VCE apply (M2 v2): the PASS-B injector multiplies the same
                            // hand value by the same α² (the factor table is refreshed once
                            // per epoch, at the end, so inject and harvest read one value),
                            // so `fold_var` here is the variance the fold used.  Both
                            // consumers need that: the census standardizes by the used
                            // weight, and the monitor's sigma_hat_post reads 1 at
                            // convergence only if its denominator is the used weight too.
                            // The hand value is passed alongside because it is the factor
                            // iteration's own denominator and has to stay independent of
                            // α².  The factor is 1 when off.
                            const double fold_var =
                                fold_var_hand * ctx.carry.vce_apply.factor(
                                                    NetworkCarryVceApply::kPl, pa.sat);
                            // Open issue (a separate card): the census numerator.  `innov`
                            // is the raw carried-minus-solved difference.  Under the card-1
                            // conditional coupling the carried prior's RHS is not
                            // `value_abs` alone: the coupling rows add Σ M_rj·ŝ_prev,j to
                            // it (the `rhs += cs.coeff * cs.s_prev` accumulation in
                            // NetworkCoupling.cpp), so on a coupled posture this ê and the
                            // ê the fold saw differ by the regressor term.  The denominator
                            // side of the same mismatch produces the r_negative population
                            // (post_var > fold_var, because the harvested posterior is a
                            // conditional variance), which the census re-partitions into
                            // its own RNEG class instead of letting it head the ladder.
                            // Aligning the numerator is not part of that change, on
                            // purpose: it would move every coupled family's ê and therefore
                            // its σ̂₀, which is solve-adjacent measurement work with its own
                            // A/B.
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
                                // The inject predicate, reproduced (see above).  A freed
                                // arc's inject was skipped (the latched set); a bridged
                                // arc's (shifted) prior did fold.
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
                // Observatory (§18.12, in the measure-first form of
                // SPEC_lane_slip_response §7.3): at the first post-resume harvest the
                // restored regressor and this epoch's freshly derived one are both in hand,
                // and their agreement is the only direct evidence that the restored row
                // was frame-valid.  In the algebra, a row with A_r = e_c, A_s = M
                // contributes α(M − M_d)(M − M_d)ᵀ to the reduced shared normals and
                // α(y − β)(M − M_d) to the shared RHS, so M == M_d makes the row exactly
                // shared-neutral, and rel ≪ 1 shows that it was.  M == 0 (the diagonal
                // restore) is the maximal deviation.  Nothing is written back:
                // byte-identical on every solve artifact.
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
                    // First-stash conditional variance: set once at the arc's birth and
                    // kept across re-stashes (the σ-scale denominator of the handover ratio
                    // ceiling; an erase-and-rebirth starts it over, and a re-level keeps it,
                    // so a refloated arc reads as unconverged until it re-earns the level).
                    const auto lkey = std::make_tuple(static_cast<int>(i), pa.sat, pa.signal);
                    const auto oldc = local_carry.find(lkey);
                    const double var_first =
                        (oldc != local_carry.end() && oldc->second.var_first > 0.0)
                            ? oldc->second.var_first : var;
                    // The arc's birth, birth_ep (the clock of the F3 block admission age
                    // gate and of the candidacy bar) and its seconds twin birth_s (the hard
                    // window floor of the sim-only slip oracle: a truth slip stamped at or
                    // before the entry's creation is already absorbed by the level the
                    // harvest created and is not re-applied), is stamped by
                    // localCarryStampBirth below, the single definition of the
                    // first-stash-wins rule (filter/NetworkRunTypes.hpp).  The first stash
                    // wins, the stamp is kept across re-stashes, and an erase-and-rebirth
                    // restarts it (with no extra clause: the entry is gone, so
                    // `oldc == end()` and this epoch is the stamp).
                    // Its "already stamped" test is `!= 0`, not `> 0` (SPEC_event_harness
                    // §15.4): a warm-resume-restored entry carries
                    // LocalCarry::kBirthEpRestored / kBirthSRestored so the rule can tell
                    // "restored, true age not snapshotted ⇒ mature" from "never stashed ⇒
                    // stamp it now".  With `> 0`, the first post-resume harvest would
                    // re-stamp every restored arc as born at the resume epoch, and every
                    // age gate would then see a network of newborns for min_age epochs.
                    // Age-aware young-arc inflation stamp (SPEC_amb_iono_split_overcount
                    // §6.17–§6.18).  Two events, and only these two, produce a level whose
                    // value is code-quality while its harvested variance is phase-stiff:
                    //   * the entry is created: a first stash, or the re-stash after a T4
                    //     attribution erase or gap prune deleted the entry (the erase
                    //     path needs no separate clause because it deletes);
                    //   * the arc was freed this epoch, so this harvest re-levelled it
                    //     from one epoch of code.
                    // The freed predicate is the harvest's own `relevel_freed` above,
                    // re-read here because that one lives inside the VCE/census scope:
                    // `relevel.free_ambs` is the latched set the PASS-B inject consulted
                    // (empty when network.cycle_slip.relevel_free_epoch is off, so no knob
                    // guard is needed, the same rule the census uses).  Every other
                    // re-stash preserves the stamp, so the age keeps growing while the arc
                    // matures.  This is not birth_s: a re-level keeps birth_s (by design,
                    // the slip oracle's floor), and it is the re-level, not the birth,
                    // that resets what the level is worth.
                    const bool yng_created = (oldc == local_carry.end());
                    const bool yng_freed =
                        ctx.carry.relevel.free_ambs.count(
                            {static_cast<int>(i), pa.amb_key}) > 0;
                    LocalCarry nlc{value_abs, var, pl_now_s, std::move(coupling)};
                    nlc.var_first = var_first;
                    // The birth prior in force (RF-1 §3.3(b),
                    // docs/refound/RF-1_float_ambiguities_design.md): var_first floored by
                    // the posture's code-noise scale, set once at birth and kept across
                    // every re-stash and re-level as var_first is (an erase-and-rebirth
                    // restarts it, because the entry is gone).  The floor defaults to 0.0,
                    // which makes this var_first to the ULP; the drop (NetworkAmbFloat.cpp)
                    // restores max(var_birth, var_first, floor).  It is maintained here
                    // rather than at the drop site because what the arc was worth at birth
                    // is a property of the birth, and the harvest is the only place that
                    // sees an arc's first stash.
                    // The floor is elevation-mapped as the code row weights are
                    // (σ²(e) = σ²_zen / sin^{2p}(e), RF-1 step 4): a flat zenith floor would
                    // be optimistic at the low elevations that hold the metre-plus tail
                    // (§24.13 finding 2).  pa.elev_deg is −1 when the floor is unarmed or
                    // the station's modeled set had no elevation for this satellite, and
                    // ambFloatBirthFloor reads that as zenith.
                    const double amb_floor_los =
                        ambFloatBirthFloor(amb_birth_var_floor, pa.elev_deg,
                                           amb_elev_power);
                    nlc.var_birth =
                        (oldc != local_carry.end() && oldc->second.var_birth > 0.0)
                            ? oldc->second.var_birth
                            : std::max(var_first, amb_floor_los);
                    if (oldc == local_carry.end() && amb_floor_los > var_first)
                        ++ctx.counters.n_amb_birth_floored;
                    // The drop stamp (RF-1 §7.2) is written by the drop
                    // (NetworkAmbFloat.cpp) and carried forward unchanged by the harvest.
                    nlc.dropped_ep =
                        (oldc != local_carry.end()) ? oldc->second.dropped_ep : 0;
                    // Walk metric (RF-3a, A2).  This is the one site that sees the
                    // pre-fold carried value and the post-fold stash together, so it is
                    // where the amount the first fold after a drop absorbed can be
                    // measured.  Measure-only, maintained whenever the vehicle is armed and
                    // in both states of network.amb_float_drop_var_m2: an off run has to
                    // report its walk for the on run's number to be comparable (standing
                    // invariant #4).
                    nlc.drop_walk_ph =
                        (oldc != local_carry.end()) ? oldc->second.drop_walk_ph : 0;
                    if (nlc.drop_walk_ph > 0 && oldc != local_carry.end()) {
                        const double dv = std::abs(value_abs - oldc->second.value_abs);
                        if (std::isfinite(dv)) {
                            if (nlc.drop_walk_ph == 1)
                                ctx.counters.amb_walk_fold1_m.push_back(dv);
                            else
                                ctx.counters.amb_walk_fold2_m.push_back(dv);
                        }
                        nlc.drop_walk_ph = (nlc.drop_walk_ph == 1) ? 2 : 0;
                    }
                    localCarryStampBirth(
                        nlc, (oldc != local_carry.end() ? &oldc->second : nullptr),
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    nlc.relevel_s = NetworkYoungInflate::stamp(
                        yng_created ? 0.0 : oldc->second.relevel_s,
                        yng_created, yng_freed, pl_now_s);
                    if (yng_created)     ++ctx.counters.n_yng_stamp_create;
                    else if (yng_freed)  ++ctx.counters.n_yng_stamp_free;
                    // The arc's trajectory row (network.trace_amb_state, RF-1 §7.4),
                    // written from the one site that sees the pre-stash entry and the
                    // post-stash value together.  Measure-only: nothing below is read back,
                    // and when off the whole block is unreachable.
                    //
                    // `folded` / `route` / `fold_var` are not re-derived here: they are the
                    // record PASS B wrote into R.amb_state_fold at the site that emitted
                    // the row, so a joint-block fallback or a non-finite-RHS screen on u_r
                    // reads as what happened, not as what the knob posture implies.
                    // n_folds counts only the epochs in which a prior acted, so it cannot
                    // be derived from birth_ep: the epochs it skips (the free-epoch latch,
                    // the gap cliff, a pending reseed) are the ones the trace exists to
                    // show.
                    long long nlc_n_folds =
                        (oldc != local_carry.end()) ? oldc->second.n_folds : 0;
                    if (amb_state_on) {
                        const auto ff = R.amb_state_fold.find({pa.sat, pa.signal});
                        const bool did_fold = (ff != R.amb_state_fold.end());
                        if (did_fold) ++nlc_n_folds;
                        AmbStateRow row;
                        row.week    = static_cast<long long>(std::floor(pl_now_s / 604800.0));
                        row.tow     = std::fmod(pl_now_s, 604800.0);
                        row.station = stations[i].name;
                        row.sat     = pa.sat;
                        row.signal  = pa.signal;
                        row.value_abs_m  = value_abs;
                        row.var_m2       = var;
                        row.var_first_m2 = nlc.var_first;
                        row.var_birth_m2 = nlc.var_birth;
                        row.fold_var_m2  = did_fold ? ff->second.first : 0.0;
                        row.folded       = did_fold;
                        row.route        = did_fold ? ff->second.second : AmbFoldRoute::None;
                        row.n_folds      = nlc_n_folds;
                        // Age reads the stamps after localCarryStampBirth, so a restored
                        // arc reports the −1 sentinel instead of being re-stamped as a
                        // newborn (SPEC_event_harness §15.4), and an unstamped relevel
                        // clock reports −1 = "mature", not "age 0".
                        row.age_ep = (nlc.birth_ep > 0)
                            ? (static_cast<long long>(ctx.counters.n_loop_epochs) - nlc.birth_ep)
                            : nlc.birth_ep;
                        row.age_since_relevel_ep =
                            (nlc.relevel_s > 0.0 && amb_state_dt_ep_s > 0.0)
                                ? static_cast<long long>(
                                      std::llround((pl_now_s - nlc.relevel_s)
                                                   / amb_state_dt_ep_s))
                                : -1;
                        // The event vocabulary is design §7.4's, also used on the level
                        // vehicle for its response paths, so an RF-0 baseline and an RF-1
                        // arm compare row for row: a `drop:` here is an inject that did
                        // not happen because the response skipped it, which is what RF-1
                        // replaces with a bounded drop.  Precedence follows the inject's
                        // own order of tests.
                        const bool reseed_now =
                            stations[i].sm->isAmbiguityPendingReseed(pa.amb_key);
                        const double gap_now = (oldc != local_carry.end())
                            ? (pl_now_s - oldc->second.last_seen_s) : 0.0;
                        if (yng_created)          row.event = "birth";
                        else if (reseed_now)      row.event = "drop:reseed";
                        else if (yng_freed)       row.event = "drop:relevel_free";
                        else if (gap_now > pl_max_gap_s) row.event = "drop:stale_gap";
                        // The stride gates trajectory rows; an event row is always
                        // written, so D3's inventory is complete at any stride.
                        if (amb_state_tr || !row.event.empty())
                            ctx.carry.amb_state_rows.push_back(std::move(row));
                    }
                    nlc.n_folds = nlc_n_folds;
                    local_carry[lkey] = std::move(nlc);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pa.col, SharedHistoryMap::Amb,
                                                  static_cast<int>(i), pa.sat, pa.signal);   // card SCL2 X2; no-op off
                }
                if (hblk && cross.size() == hblk->C.rows()) {
                    // Local block (F3): row a of the station's harvest block is this
                    // column's n_c read at every carried column (its own entry equals
                    // `var`); the entry points at the block and its index, so the next
                    // inject can pair it with any sibling stashed at this harvest, and
                    // with nothing else.
                    hblk->C.row(static_cast<Eigen::Index>(a)) = cross.transpose();
                    auto& ent = local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)];
                    ent.blk = hblk;
                    ent.blk_idx = static_cast<int>(a);
                }
                // One-shot IF-phase refloat: the fresh post-slip value is now stashed, so
                // drop any pending-reseed mark and let the next epoch resume normal carry
                // on the new arc.  Cleared only after a successful stash: a skipped
                // non-finite value keeps the mark, so the stale pre-slip carry is not
                // trusted again.  No-op when unmarked.
                stations[i].sm->clearPendingReseed(pa.amb_key);
            }
            // Card AIL (network.trace_amb_if, trace class): the ionosphere-free ambiguity's
            // variance before and after this epoch, per line of sight whose two bands are
            // both carried in the block.  Prior: the joint covariance the inject folded
            // (R.pl_blk.Sigma, rows keyed by local column); posterior: the harvest block
            // just filled (rows a of R.pl_amb).  Both are conditional on the border.  The
            // demeaned versions project out the station's common mode over the traced lines
            // of sight of the same constellation (the direction the free receiver clock and
            // that constellation's receiver biases leave to the code).  Reads only; nothing
            // the solve uses is written.
            if (ctx.knobs.amb_if_trace && hblk && !R.pl_amb.empty()) {
                // The ionosphere-free pair of a line of sight: the first two bands of its
                // constellation in RINEX digit order (the traits table: GPS L1/L2, Galileo
                // E1/E5a, QZSS L1/L2, SBAS L1/L5), with the IF coefficients from their
                // carriers.  BeiDou is not traced: its first two bands, B1C and B1I, are
                // 14 MHz apart and form no usable pair (the coefficient is about 55); its
                // pair is a band-role decision of card NOM step N3.  GLONASS is never
                // carried in the block; its FDMA bands would be read at slot 0.
                struct IfPair { char d1{'\0'}, d2{'\0'}; double ca{0.0}, cb{0.0}; };
                const auto ifPairOf = [](System sys) {
                    IfPair pr;
                    const ConstellationTraits& t = traitsOf(sys);
                    if (sys == System::BeiDou || t.n_bands < 2) return pr;
                    const double f1 = nominalCarrierFrequencyHz(t.bands[0].band);
                    const double f2 = nominalCarrierFrequencyHz(t.bands[1].band);
                    pr.d1 = t.bands[0].digit;
                    pr.d2 = t.bands[1].digit;
                    pr.ca = f1 * f1 / (f1 * f1 - f2 * f2);
                    pr.cb = -f2 * f2 / (f1 * f1 - f2 * f2);
                    return pr;
                };
                std::map<std::string, std::pair<int, int>> los;   // sat -> (a band 1, a band 2)
                std::map<std::string, IfPair> if_pair;            // sat -> its pair
                for (std::size_t a = 0; a < R.pl_amb.size(); ++a) {
                    const System sys = systemOfSat(R.pl_amb[a].sat);
                    const auto sig = SignalId::fromObsCode(sys, R.pl_amb[a].signal);
                    if (!sig) continue;
                    const IfPair& pr =
                        if_pair.try_emplace(R.pl_amb[a].sat, ifPairOf(sys)).first->second;
                    if (pr.d1 == '\0') continue;
                    auto& lp = los.try_emplace(R.pl_amb[a].sat, -1, -1).first->second;
                    if (sig->digit == pr.d1 && lp.first < 0) lp.first = static_cast<int>(a);
                    else if (sig->digit == pr.d2 && lp.second < 0) lp.second = static_cast<int>(a);
                }
                std::map<Eigen::Index, Eigen::Index> prow;   // local column -> prior block row
                for (std::size_t k = 0; k < R.pl_blk.rows.size(); ++k)
                    prow[R.pl_blk.rows[k].local_col] = static_cast<Eigen::Index>(k);
                // The traced lines of sight, one group per constellation: each constellation
                // has its own receiver code and phase biases, so its common mode is its own.
                // `los` is in satellite-string order, so a constellation is a contiguous run.
                struct TracedLos { std::string sat; std::array<int, 2> idx; IfPair coef; };
                std::vector<std::vector<TracedLos>> groups;
                System group_sys = System::Unknown;
                for (const auto& [sat, lp] : los) {
                    if (lp.first < 0 || lp.second < 0) continue;
                    const System sys = systemOfSat(sat);
                    if (groups.empty() || sys != group_sys) {
                        groups.emplace_back();
                        group_sys = sys;
                    }
                    groups.back().push_back({sat, {lp.first, lp.second}, if_pair.at(sat)});
                }
                const auto post = [&](int a, int b) {
                    const double u = hblk->C(a, b), v = hblk->C(b, a);
                    return std::isfinite(u) && std::isfinite(v) ? 0.5 * (u + v)
                         : std::numeric_limits<double>::quiet_NaN();
                };
                const auto prior = [&](int a, int b) {
                    const auto ia = prow.find(R.pl_amb[static_cast<std::size_t>(a)].col);
                    const auto ib = prow.find(R.pl_amb[static_cast<std::size_t>(b)].col);
                    if (ia == prow.end() || ib == prow.end())
                        return std::numeric_limits<double>::quiet_NaN();
                    return R.pl_blk.Sigma(ia->second, ib->second);
                };
                const double t_week = std::floor(pl_now_s / 604800.0);
                const double t_tow  = pl_now_s - 604800.0 * t_week;
                for (const std::vector<TracedLos>& g : groups) {
                    const Eigen::Index n = static_cast<Eigen::Index>(g.size());
                    const auto ifcov = [&](const auto& c, Eigen::Index r, Eigen::Index s) {
                        const auto& p = g[static_cast<std::size_t>(r)].idx;
                        const auto& q = g[static_cast<std::size_t>(s)].idx;
                        const IfPair& u = g[static_cast<std::size_t>(r)].coef;
                        const IfPair& v = g[static_cast<std::size_t>(s)].coef;
                        return u.ca * v.ca * c(p[0], q[0]) + u.ca * v.cb * c(p[0], q[1])
                             + u.cb * v.ca * c(p[1], q[0]) + u.cb * v.cb * c(p[1], q[1]);
                    };
                    Eigen::MatrixXd Vpo(n, n), Vpr(n, n);
                    for (Eigen::Index r2 = 0; r2 < n; ++r2)
                        for (Eigen::Index c2 = 0; c2 < n; ++c2) {
                            Vpo(r2, c2) = ifcov(post, r2, c2);
                            Vpr(r2, c2) = ifcov(prior, r2, c2);
                        }
                    const Eigen::MatrixXd Pm = Eigen::MatrixXd::Identity(n, n)
                        - Eigen::MatrixXd::Constant(n, n, 1.0 / static_cast<double>(n));
                    const bool po_ok = Vpo.allFinite(), pr_ok = Vpr.allFinite();
                    const Eigen::MatrixXd Dpo = po_ok ? Eigen::MatrixXd(Pm * Vpo * Pm) : Vpo;
                    const Eigen::MatrixXd Dpr = pr_ok ? Eigen::MatrixXd(Pm * Vpr * Pm) : Vpr;
                    for (Eigen::Index r2 = 0; r2 < n; ++r2) {
                        const TracedLos& t = g[static_cast<std::size_t>(r2)];
                        const auto lit = local_carry.find(std::make_tuple(
                            static_cast<int>(i), t.sat,
                            R.pl_amb[static_cast<std::size_t>(t.idx[0])].signal));
                        const long long age = (lit != local_carry.end() && lit->second.birth_ep > 0)
                            ? static_cast<long long>(ctx.counters.n_loop_epochs) - lit->second.birth_ep
                            : -1;
                        ctx.carry.amb_if_rows.push_back(fmt::format(
                            "{:.0f},{:.1f},{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e}\n",
                            t_week, t_tow, stations[i].name, t.sat,
                            age, n, Vpr(r2, r2), Vpo(r2, r2),
                            pr_ok ? Dpr(r2, r2) : std::numeric_limits<double>::quiet_NaN(),
                            po_ok ? Dpo(r2, r2) : std::numeric_limits<double>::quiet_NaN()));
                    }
                }
            }
            // Card LGM: stash each phase-colour column's posterior (the estimate, its pure
            // conditional variance, its border regression) and its row of the station's
            // harvest block, so the next epoch folds it jointly with the ambiguities it
            // shares the phase rows with.  The coupling carry is the posture's (the
            // loader requires it); a column whose regression fails is not stashed and is
            // born again next epoch (counted).
            if (pc_in_cols && carry_cpl_cond) {
                const double t_week_pc = std::floor(pl_now_s / 604800.0);
                const double t_tow_pc  = pl_now_s - 604800.0 * t_week_pc;
                for (std::size_t k = 0; k < R.pl_pc.size(); ++k) {
                    const PlPc& pp = R.pl_pc[k];
                    if (pp.col < 0 || pp.col >= xr.size()) { ++ctx.counters.n_pc_stash_fail; continue; }
                    const double value_abs = pp.x_lin + xr(pp.col);
                    Eigen::VectorXd pcross;
                    auto hc = harvestCoupling(est_idx, pp.col, hblk ? &cols : nullptr,
                                              hblk ? &pcross : nullptr);
                    const double var = hc.first;
                    if (!std::isfinite(value_abs) || !(var > 0.0) || !std::isfinite(var)) {
                        ++ctx.counters.n_pc_stash_fail;
                        continue;
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(hc.second)};
                    const Eigen::Index prow = pc_row0 + static_cast<Eigen::Index>(k);
                    if (hblk && pcross.size() == hblk->C.rows() && prow < hblk->C.rows()) {
                        hblk->C.row(prow) = pcross.transpose();
                        ent.blk     = hblk;
                        ent.blk_idx = static_cast<int>(prow);
                    }
                    ctx.carry.pc_carry[{static_cast<int>(i), pp.sat}] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pp.col, SharedHistoryMap::Pc,
                                                  static_cast<int>(i), pp.sat);   // card SCL2 X2; no-op off
                    ++ctx.counters.n_pc_stash;
                    ctx.counters.pc_abs_sum += std::abs(value_abs);
                    ctx.counters.pc_sq_sum  += value_abs * value_abs;
                    if (ctx.knobs.pc_trace)
                        ctx.carry.pc_trace_rows.push_back(fmt::format(
                            "{:.0f},{:.1f},{},{},{:.2f},{:.6f},{:.6f},{}\n",
                            t_week_pc, t_tow_pc, stations[i].name, pp.sat, pp.elev_deg,
                            value_abs, std::sqrt(var), pp.carried ? 1 : 0));
                }
            }
        }
        // ---- Lane observatory (rung 3 v3): post-event measurement -----------
        // Runs after the harvest loop, so both bands' fresh levels coexist, and before the
        // gap prune, so a pair that is about to be time-pruned is still measurable this
        // epoch.
        //
        //   delta = [C_a(t+)/lam_a - C_b(t+)/lam_b]
        //         - [C_a(t-)/lam_a - C_b(t-)/lam_b] - dM        [cycles]
        //
        // Every per-(station, satellite, pair) bias (receiver and satellite, code and
        // phase) is constant across the event and cancels in both differences, so no bias
        // convention has to be reconciled (the failure of v1), and snapped and unsnapped
        // arcs would mix freely in a DD (the v1 DD-closure failure).  Measure-only:
        // nothing is written back.
        if (lane_obs_on) {
            // The denominator is the freed set, not the raw rows: a freed arc the raw
            // pass never saw shows up as a counted skip, not as an unreported shrinkage
            // of the denominator (invariant #3: measure the deferral).
            std::set<std::pair<int, std::string>> freed_los;
            // LOS-level union of freeing causes (§7.6(c)): a satellite freed on two
            // bands by two different detectors carries both bits.
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
            // Signal-code namespace: the raw pass names a band by the lexicographically
            // first attribute it tracks (L1C), while the ambiguity / carry key uses the
            // estimator-selected code (L1W under a "W-before-C" priority list).  Matching
            // the two by full code would miss every entry on such a network and report
            // the miss as "no pre-event carry", a wrong population verdict rather than a
            // wiring alarm.  Resolve by band digit against the carry itself, which is the
            // namespace both lane levels are read from.
            const auto resolve = [&](int sta, const std::string& sat,
                                     const std::string& code) -> std::string {
                const System sys = systemOfSat(sat);
                const auto want = SignalId::fromObsCode(sys, code);
                if (!want) return {};
                auto it = local_carry.lower_bound(std::make_tuple(sta, sat, std::string{}));
                for (; it != local_carry.end(); ++it) {
                    if (std::get<0>(it->first) != sta || std::get<1>(it->first) != sat) break;
                    const std::string& sig = std::get<2>(it->first);
                    const auto have = SignalId::fromObsCode(sys, sig);
                    if (have && have->digit == want->digit) return sig;   // same band
                }
                return {};
            };
            for (const auto& los : freed_los) {
                const int  sta = los.first;
                const std::string& sat = los.second;
                ++ctx.counters.n_lane_attempts;
                // One per-event row per attempt (§7.6(a)); the skip channel is the
                // outcome, so the trace closes against the counters.
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
                // The response has to have touched this pair.  A satellite can be in
                // freed_los because a third band was freed (L5 on a triple posture, or a
                // second channel of the same band under channel_mode=all); both of this
                // pair's carries are then continuous, there is no re-level to measure, and
                // admitting the pair would fold ordinary carried-prior evolution into the
                // histogram, biasing it toward the pre-registered "mass below ~2 cyc"
                // conclusion.
                const bool a_freed =
                    ctx.carry.relevel.free_ambs.count({sta, StateKeys::makeAmbiguityKey(sat, sa)}) > 0;
                const bool b_freed =
                    ctx.carry.relevel.free_ambs.count({sta, StateKeys::makeAmbiguityKey(sat, sb)}) > 0;
                if (!a_freed && !b_freed) {
                    skip(ctx.counters.n_lane_skip_no_freed_band, "skip_no_freed_band"); continue;
                }
                // A delta-bridged band's carry was already re-based to pre_slip + shift,
                // so its lane response is the bridge's, not the free re-level's: a
                // different population, counted separately.
                if (ctx.carry.relevel.bridged.count(ka)
                    || ctx.carry.relevel.bridged.count(kb)) {
                    skip(ctx.counters.n_lane_skip_bridged, "skip_bridged"); continue;
                }
                const auto pa_it = lane_pre.find(ka), pb_it = lane_pre.find(kb);
                if (pa_it == lane_pre.end() || pb_it == lane_pre.end()) {
                    skip(ctx.counters.n_lane_skip_no_pre, "skip_no_pre"); continue;
                }
                // Pre-event freshness: exactly one processed epoch old.  Otherwise the
                // carry difference spans a different interval from the one-epoch MW step
                // it is differenced against.
                if (!(ctx.carry.pl_prev_now_s > 0.0)
                    || pa_it->second.second != ctx.carry.pl_prev_now_s
                    || pb_it->second.second != ctx.carry.pl_prev_now_s) {
                    skip(ctx.counters.n_lane_skip_stale_pre, "skip_stale_pre"); continue;
                }
                // Post-event freshness: re-stashed this epoch, tested on last_seen_s and
                // not on map presence.  An entry that was not refreshed survives a full
                // pl_max_gap_s, and accepting it would use the pre-event value as the
                // post-event value, collapsing the delta either to a one-band code
                // re-level (top bins) or to exactly -dMW (~0.35 cyc, bottom bins).  Either
                // way the histogram would show a verdict the data does not support.
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
                // Cause split over measured pairs, as bit counts (§7.6(c)).
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
                // The band-scoped subset separately (§7.6(b)), so the rung-2 contrast
                // (vertex-wide against band-scoped response) is readable.
                if (!vertex_wide) ++ctx.counters.n_lane_hist_bandscoped[bin];
                row.measured = true;
                row.vertex_wide = vertex_wide;
                row.delta_cyc = delta;
                ctx.carry.relevel.lane_obs_rows.push_back(row);
            }
            // ---- T4-side measurement over the previous epoch's pre-erase captures
            // (§7.6(d)): the RAIM attribution / consensus erase channel, which is most
            // of all field freeing and does not appear in free_ambs.  Same delta, with
            // one extra interlock: the capture is a copy across an epoch boundary, so a
            // pair whose (sat,σ) was eq34-re-referenced this epoch is skipped (the
            // detector path has that guard implicitly, because lane_pre reads
            // local_carry, which the re-reference hygiene already erased).
            // Measure-only, with its own closure.
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
                    // Resolve the captured bands by band digit against the raw pair (the
                    // capture keys carry the estimator-selected carry signal names; see
                    // the namespace note at `resolve`).
                    const auto cap_of = [&](const std::string& code)
                        -> std::pair<const std::string*, const NetworkCarryState::LaneT4Pre*> {
                        const System sys = systemOfSat(sat);
                        const auto want = SignalId::fromObsCode(sys, code);
                        if (!want) return {nullptr, nullptr};
                        for (auto it2 = t4_lo; t4_same(it2); ++it2) {
                            const std::string& sig = std::get<2>(it2->first);
                            const auto have = SignalId::fromObsCode(sys, sig);
                            if (have && have->digit == want->digit)   // same band
                                return {&std::get<2>(it2->first), &it2->second};
                        }
                        return {nullptr, nullptr};
                    };
                    const auto ca = cap_of(m.sig1);
                    const auto cb = cap_of(m.sig2);
                    if (!ca.second || !cb.second) {
                        skip(ctx.counters.n_lane_t4_skip_no_pre, "t4_skip_no_pre"); continue;
                    }
                    // Handover cross-guard (§7.7): every frame channel that can
                    // re-realize this pair's family between capture and measurement.
                    // These are the eq34 (sat,σ) re-reference, the phase-anchor
                    // election's re-referenced families (non-eq34 lineages; the set
                    // persists across withheld epochs, on purpose, until a stash
                    // commits the new frame), and the datum-station slip's network-wide
                    // reseed (per satellite, all signals).
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
                    // Post-event: re-stashed this epoch under the same signal names; an
                    // attribute re-selection after the refloat is a different lane, not a
                    // measurable response.
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
            // One-epoch lifetime (invariant #1): a capture is consumed at the first
            // harvest after its erase, or dropped; it is never measured across an
            // unequal interval.
            ctx.carry.lane_t4_pre.clear();
        }
        // Stamp this harvest so the next epoch can require a one-epoch-old pre-event
        // value.
        ctx.carry.pl_prev_now_s = pl_now_s;

        // Card LGM: prune phase-colour entries past the maximum gap.  Such an entry would
        // not be folded anyway (the inject re-births the line of sight); the prune keeps
        // the map to the live lines of sight and counts the edge.  The horizon is the
        // registry's (network.phase_colour_max_gap_s, the same double; card PRM phase 3b).
        if (ctx.knobs.pc_on)
            carryGapPrune(ctx.carry.pc_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::PhaseColour).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_pc_gap_prune;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::PcGapPrune);   // card PRM phase 3a
                          });

        // Carry-retirement census.  This prune is the mechanism behind the fleet-scale
        // carry loss that §1.3 calls the "cliff".  It reads only last_seen_s (it selects
        // on elapsed time and nothing else), so any claim that it improves quality has
        // to be measured against the quality-selected retirement paths, not assumed.
        // The fleet size is captured before the loop, so "fleet-scale" is a ratio.
        // The horizon is the registry's (network.phase_leveling_max_gap_s, the same double
        // as pl_max_gap_s; card PRM phase 3b).
        {
            const long long fleet_before = static_cast<long long>(local_carry.size());
            const long long n_pruned = carryGapPrune(
                local_carry, pl_now_s,
                ctx.params.spec(NetworkParamKind::Ambiguity).gap_horizon_s,
                [&](const auto& it) {
                    // Capture before the erase (RF-1 §7.4, D3).  This is the one
                    // retirement path that design §3.4 keeps as an erase ("this is a
                    // different arc"), so its inventory is the control against which
                    // the three that become bounded drops are read.
                    if (amb_state_on) {
                        const int sidx = std::get<0>(it->first);
                        ambStateNoteErase(
                            ctx,
                            static_cast<long long>(std::floor(pl_now_s / 604800.0)),
                            std::fmod(pl_now_s, 604800.0),
                            (sidx >= 0 && sidx < static_cast<int>(stations.size()))
                                ? stations[sidx].name : std::string{},
                            std::get<1>(it->first), std::get<2>(it->first),
                            it->second, "gap_prune");
                    }
                    // The decoupling edge of the carry itself (card SID phase 3, R3a).
                    // The key is buffered for the roster's audit pass, which owns the
                    // trace bundle and the epoch time and writes the
                    // `ARC arc_death reason=horizon` row; without that, this retirement
                    // would emit nothing.  Untouched without the observatory: the vector
                    // stays empty and nothing reads it.
                    if (ctx.roster.armed)
                        ctx.carry.sid_carry_prune_deaths.push_back(it->first);
                    // Card PRM phase 3a: an arc the float vehicle's drop already dropped
                    // this epoch (networkAmbFloatApplyDrops stamps dropped_ep with this
                    // epoch's n_loop_epochs before PASS B; this harvest runs later in the
                    // same epoch, and the counter does not move in between).
                    if (it->second.dropped_ep > 0
                        && it->second.dropped_ep
                               == static_cast<long long>(ctx.counters.n_loop_epochs))
                        ++ctx.counters.prm_pruned_after_drop;
                });
            if (n_pruned > 0) {
                ctx.counters.n_cr_gap_prune += n_pruned;
                prmRespond(ctx.counters.prm_resp, ResponseSite::AmbGapPrune, n_pruned);   // card PRM phase 3a
                ++ctx.counters.n_cr_gap_prune_epochs;
                if (n_pruned > ctx.counters.n_cr_gap_prune_max) {
                    ctx.counters.n_cr_gap_prune_max       = n_pruned;
                    // Week/TOW, not pl_now_s: every trace CSV and log line in the run is
                    // keyed on TOW, so a continuous-GPS-seconds handle (~1.4e9) would not
                    // match anything a reader can search for.
                    ctx.counters.n_cr_gap_prune_max_week  =
                        static_cast<long long>(std::floor(pl_now_s / 604800.0));
                    ctx.counters.cr_gap_prune_max_tow     = std::fmod(pl_now_s, 604800.0);
                    ctx.counters.n_cr_gap_prune_max_fleet = fleet_before;
                }
            }
        }
        // Retap statistics-guard set: one RAIM-to-harvest lifetime, consumed by the
        // labeling above and cleared here, so next epoch's RAIM refills it.
        ctx.carry.relevel_retapped_prev.clear();
        // --- In-block slant-iono carry (network.carry_iono_in_block): harvest each
        //     station's per-LOS iono posteriors through the same per-column solve the
        //     ambiguities used, write their rows of the station's harvest block, stash
        //     the absolute value, and prune the stale entries.
        //
        //     Independent loop (as for the ZWD), so it runs for stations whose
        //     ambiguity harvest produced nothing.  It differs from the harvest of the
        //     diagonal slant-iono carry (queue 8(b)) in three deliberate ways:
        //       * the cross terms are gathered (they subtract the B-fold over-count at
        //         inject); queue 8(b) took the diagonal alone;
        //       * the coupling row is harvested: an in-block column is folded like an
        //         ambiguity, border regression included, which makes the prior exact
        //         given the border (the rx-CB "no shared footprint" rationale applied to
        //         the diagonal vehicle, not to this one);
        //       * the value is absolute (x_lin + x_r).  network_ionosphere.csv writes
        //         x_r alone and is not frame-complete; a carry that is not
        //         frame-complete would re-inject a correction against a moved
        //         linearization point.
        //     A LOS whose harvest fails keeps blk = nullptr, so every pair with it is
        //     zeroed and counted (n_pl_blk_iono_missing_cross), which is the safe
        //     degrade.
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
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
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
                    // on an erase-and-rebirth (a T4 erase, or a gap prune): the ambiguity
                    // carry's rule, applied through the ambiguity carry's own helper, so
                    // the age of an iono carry means the same thing.  (This map is not
                    // snapshotted, so it never sees a restored sentinel; the shared helper
                    // keeps the rule correct if it ever is.)
                    const auto oit = iono_blk_carry.find(key);
                    const LocalCarry* iprev =
                        (oit != iono_blk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = iprev ? iprev->var_first : var;
                    localCarryStampBirth(
                        ent, iprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row irow0 + a of the station's harvest block is this iono column's
                    // n_c read at every carried column of the station (its own entry
                    // equals `var`, the pure conditional).  The entry then points at the
                    // same PlHarvestBlock as the ambiguity and ZWD entries, so next
                    // epoch's inject pairs iono↔ambiguity, iono↔ZWD and iono↔iono by
                    // pointer equality, as it pairs ambiguity↔ambiguity.
                    if (iblk && irow0 >= 0 && icross.size() == iblk->C.rows()) {
                        const Eigen::Index irow = irow0 + static_cast<Eigen::Index>(a);
                        if (irow < iblk->C.rows()) {
                            iblk->C.row(irow) = icross.transpose();
                            ent.blk     = iblk;
                            ent.blk_idx = static_cast<int>(irow);
                        }
                    }
                    iono_blk_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, icol, SharedHistoryMap::Iono,
                                                  static_cast<int>(i), isat);   // card SCL2 X2; no-op off
                }
            }
            // Decoupling edge (invariants #1 and #7): a LOS unseen past the horizon is
            // dropped, not aged into a fold.  It is counted, because this is the
            // eclipse / set edge, and a run whose count is 0 over a full day did not
            // exercise it.  The horizon is the registry's: under the block carry it is
            // network.carry_iono_in_block_max_gap_s, the same double
            // (ctx.knobs.iono_blk_max_gap_s) this prune read before card PRM phase 3b.
            carryGapPrune(iono_blk_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::SlantIono).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_iono_blk_gap_drop;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::IonoGapPrune);   // card PRM phase 3a
                          });
        }
        // --- In-block receiver-clock carry (network.carry_rx_clock_in_block): harvest
        //     each station's receiver-clock (and ISB) posteriors through the same
        //     per-column solve the ambiguities used, write their rows of the station's
        //     harvest block, run the (bias, drift) filter, and prune the stale entries.
        //
        //     The drift is central to the design.  The producer never writes its
        //     solution back into the station StateManager, so x_lin(clock) is set once
        //     (0, or the latched cold_clock_seed median) and is constant for the run:
        //     x_r carries the entire clock, including a ramp of about 0.3 m/s, i.e. 9 m
        //     per 30 s epoch (0.29994 m/s on the sim truth bed, with 0.299 m random-walk
        //     increments about it).  A bias-only random walk at q = 0.09 m² would be
        //     about 30 σ wrong every epoch.  The carry is therefore the (bias, drift)
        //     pair: the block column holds the bias, and the drift is a per-station
        //     scalar Kalman state updated here from the harvested increments, the same
        //     state that supplies the one-step prediction whose innovation is the
        //     clock-jump detector.
        //
        //     Independent loop (as for the ZWD and iono), so it runs for stations whose
        //     ambiguity harvest produced nothing.  ISB columns share the loop but not
        //     the drift: an inter-system bias is a near-constant offset (the sim
        //     injects a fixed 8.000 m), so it takes a pure random walk.
        if (rxclk_blk_on) {
            auto& rx_clk_carry = ctx.carry.rx_clk_carry;
            auto& rx_clk_rate  = ctx.carry.rx_clk_rate;
            // The run's own observed harvest-to-harvest spacing, kept here rather than
            // read from ctx.carry.pl_prev_now_s, because that field has already been
            // advanced to pl_now_s when this loop runs.  Used only to convert a gap into
            // epochs for the per-epoch q, so the knob means the same thing at 1 s and at
            // 30 s.
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
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
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
                    // --- (bias, drift) filter and the clock-jump response, for the primary
                    //     clock only (pr.kind empty); an ISB has no rate by construction.
                    if (pr.kind.empty()) {
                        auto& rt = rx_clk_rate[key];   // default-constructs on first sighting
                        if (rt.prev_s > 0.0) {
                            const double dt = pl_now_s - rt.prev_s;
                            if (dt > 0.0 && dt <= rxclk_gap_s) {
                                const double gap_ep = (dt_ep_run > 0.0)
                                                          ? std::max(1.0, dt / dt_ep_run) : 1.0;
                                // rate_var < 0 = "never updated": the drift is unknown, so
                                // the prediction is bias-only with an uninformative rate.
                                // This also keeps the jump gate correct on the first
                                // increment: with σ_d = 1 m/s the 30 s prediction band is
                                // about 30 m, which a 9 m/epoch drift passes at 0.3 σ and a
                                // 1 ms receiver step (3e5 m) fails by four orders of
                                // magnitude.
                                const double rvar_eff =
                                    (rt.rate_var >= 0.0) ? rt.rate_var : rxclk_rate_var0;
                                // The same kernel PASS B injected with, so the innovation
                                // is tested against the prediction that was used.
                                const RxClkPred rp = rxClockPredict(
                                    rt.prev_abs, rt.prev_var, rt.rate, rvar_eff, dt,
                                    gap_ep, rxclk_q);
                                const double var_pred = rp.var;
                                const double innov    = value_abs - rp.mean;
                                // Gate-scale census (invariant #4): the gate is
                                // k*sqrt(var_pred) metres and var_pred is per
                                // station-epoch, so a report can quote the size an event
                                // has to exceed rather than only k.  Measure-only.
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
                                    // The receiver (or the solve) stepped (invariants #1
                                    // and #7).  Decoupling edge: erase both halves, so
                                    // nothing crosses the jump.  Re-coupling edge: the next
                                    // sighting is a cold rebirth whose rate prior is
                                    // uninformative again, so the two epochs after a jump
                                    // inject a nearly free prior rather than a stale one.
                                    const std::size_t prm_n_clk = rx_clk_carry.erase(key);
                                    rx_clk_rate.erase(key);
                                    ++ctx.counters.n_rxclk_carry_refused;
                                    // Card PRM phase 3a: the carried clock the jump erased.
                                    prmRespond(ctx.counters.prm_resp, ResponseSite::RxclkJumpErase,
                                               static_cast<long long>(prm_n_clk));
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
                    // birth_ep / birth_s / var_first survive a re-stash and restart only
                    // on an erase-and-rebirth (a jump response, or a gap prune): the
                    // ambiguity carry's rule, applied through the ambiguity carry's own
                    // helper, so the age of a clock carry means the same thing.  (This map
                    // is not snapshotted either; see the note in the iono block above.)
                    const auto oit = rx_clk_carry.find(key);
                    const LocalCarry* rprev =
                        (oit != rx_clk_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = rprev ? rprev->var_first : var;
                    localCarryStampBirth(
                        ent, rprev,
                        static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row rrow0 + a of the station's harvest block is this clock-family
                    // column's n_c read at every carried column of the station (its own
                    // entry equals `var`, the pure conditional).  The entry then points at
                    // the same PlHarvestBlock as the ambiguity / iono / ZWD entries, so
                    // next epoch's inject pairs clock↔ambiguity, clock↔iono and clock↔ZWD
                    // by pointer equality, as it pairs ambiguity↔ambiguity.
                    if (rblk && rrow0 >= 0 && rcross.size() == rblk->C.rows()) {
                        const Eigen::Index rrow = rrow0 + static_cast<Eigen::Index>(a);
                        if (rrow < rblk->C.rows()) {
                            rblk->C.row(rrow) = rcross.transpose();
                            ent.blk     = rblk;
                            ent.blk_idx = static_cast<int>(rrow);
                        }
                    }
                    rx_clk_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, pr.col, SharedHistoryMap::RxClk,
                                                  static_cast<int>(i), pr.kind);   // card SCL2 X2; no-op off
                }
            }
            // Decoupling edge (invariants #1 and #7): a station unseen past the horizon
            // is dropped, not aged into a fold, and its drift state goes with it, because
            // extrapolating a rate across an unobserved hour is the stale-prior failure
            // the carry is designed to avoid.  Counted.  The horizon is the registry's
            // (network.carry_rx_clock_max_gap_s, the same double as rxclk_gap_s; card PRM
            // phase 3b).
            carryGapPrune(rx_clk_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::RxClock).gap_horizon_s,
                          [&](const auto& it) {
                              rx_clk_rate.erase(it->first);
                              ++ctx.counters.n_rxclk_carry_gap_drop;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::RxclkGapPrune);   // card PRM phase 3a
                          });
            ctx.carry.rxclk_prev_now_s = pl_now_s;
        }
        // --- RXCB-IN-BLOCK (card CTU section 14): harvest the receiver code biases -
        //     with their border regression and their row of the station's harvest
        //     block, so next epoch's inject folds them as coupling rows of the same
        //     whitened block as the ambiguities.  The value is the posterior absolute
        //     (x_lin + x_r), the variance the pure conditional (N_rr^-1)_cc, exactly as
        //     for every other block column.  The standalone diagonal harvest further
        //     down skips its stash under this knob (it still sums the produce counters
        //     and runs the gap prune).
        if (rxcb_blk_on) {
            auto& rx_cb_carry = ctx.carry.rx_cb_carry;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_cb.empty()) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                const std::shared_ptr<PlHarvestBlock> cblk = cblk_of_sta[i];
                const std::vector<Eigen::Index>* ccross_cols =
                    cblk ? &cblk_cols_of_sta[i] : nullptr;
                const Eigen::Index crow0 = cblk ? cblk_row0_of_sta[i] : Eigen::Index{-1};
                Eigen::VectorXd xr;
                try { xr = est.reconstructStation(est_idx, x_s); } catch (...) { continue; }
                for (std::size_t a = 0; a < R.pl_cb.size(); ++a) {
                    const auto& [cb_key, ccol, cx_lin] = R.pl_cb[a];
                    if (ccol < 0 || ccol >= xr.size()) continue;
                    const double value_abs = cx_lin + xr(ccol);
                    if (!std::isfinite(value_abs)) continue;
                    Eigen::VectorXd ccross;
                    auto hc = harvestCoupling(est_idx, ccol, ccross_cols,
                                              cblk ? &ccross : nullptr);
                    const double var = hc.first;
                    if (!(var > 0.0) || !std::isfinite(var)) {
                        ++ctx.counters.n_rxcb_blk_harvest_fail;
                        continue;
                    }
                    const auto key = std::make_pair(static_cast<int>(i), cb_key);
                    // Event response (invariant #1): the step between this harvest's
                    // absolute and the carried one, against both variances, the random
                    // walk over the gap and the diagonal route's 0.5 m guard, at 10 sigma.
                    // Frame-invariant (two absolute realizations), unlike the inject
                    // screen it replaces.  A step erases the entry (reset, not repair):
                    // this epoch folded the stale prior, so its posterior is suspect too,
                    // and the next harvest re-seeds from a free solve.
                    {
                        const auto sit = rx_cb_carry.find(key);
                        if (sit != rx_cb_carry.end()) {
                            const double sgap = pl_now_s - sit->second.last_seen_s;
                            if (sgap >= 0.0 && sgap <= kRxCbMaxGapS) {
                                const double sv = var + sit->second.var
                                                  + ctx.knobs.rx_cb_q * sgap + 0.25;
                                if (std::abs(value_abs - sit->second.value_abs)
                                    > 10.0 * std::sqrt(sv)) {
                                    rx_cb_carry.erase(sit);
                                    ++ctx.counters.n_rxcb_blk_step_reset;
                                    prmRespond(ctx.counters.prm_resp, ResponseSite::RxcbStepErase);   // card PRM phase 3a
                                    continue;
                                }
                            }
                        }
                    }
                    LocalCarry ent{value_abs, var, pl_now_s, std::move(hc.second)};
                    const auto oit = rx_cb_carry.find(key);
                    const LocalCarry* cprev = (oit != rx_cb_carry.end()) ? &oit->second : nullptr;
                    ent.var_first = cprev ? cprev->var_first : var;
                    localCarryStampBirth(
                        ent, cprev, static_cast<long long>(ctx.counters.n_loop_epochs), pl_now_s);
                    // Row crow0 + a of the station's harvest block is this column's n_c read
                    // at every carried column of the station, so next epoch's block pairs
                    // it with the ambiguities by pointer equality.
                    bool in_blk = false;
                    if (cblk && crow0 >= 0 && ccross.size() == cblk->C.rows()) {
                        const Eigen::Index crow = crow0 + static_cast<Eigen::Index>(a);
                        if (crow < cblk->C.rows()) {
                            cblk->C.row(crow) = ccross.transpose();
                            ent.blk     = cblk;
                            ent.blk_idx = static_cast<int>(crow);
                            in_blk = true;
                        }
                    }
                    if (!in_blk) ++ctx.counters.n_rxcb_blk_no_block;
                    rx_cb_carry[key] = std::move(ent);
                    networkSharedHistoryNoteEntry(shx_h, est_idx, ccol, SharedHistoryMap::RxCb,
                                                  static_cast<int>(i), cb_key);   // card SCL2 X2; no-op off
                    ++ctx.counters.n_rxcb_blk_harvested;
                    ++ctx.carry.rx_cb_harvested_total;
                }
            }
        }
        // --- ZWD carry (B9): harvest the posterior ZWD and stash it (mirrors the
        //     ambiguity harvest, in an independent loop so it works whether or not
        //     ambiguities are present).  Runs for B9 proper, for a live post-resume
        //     seam window (rolling B9), and, capture-only with nothing fed back, on any
        //     run with a snapshot path, so every RTB-7 snapshot serializes fresh Z rows
        //     (§17 finding 3).  The capture path is read-only with respect to the
        //     solve: R.pl_zwd_present is record-only on those epochs and no inject
        //     fires.
        if (zwd_on || zwd_capture || zwd_seam_live) {
            for (size_t i = 0; i < stations.size(); ++i) {
                // Harvest the pivot's ZWD too (B9): R.pl_zwd_present is set for the pivot
                // because the carry inject also runs for it.
                if (est_index_of[i] < 0) continue;
                const StationAssembly& R = asm_res[i];
                if (!R.pl_zwd_present) continue;
                const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(est_idx, x_s);
                    if (!carry_cpl_cond)   // the coupling carry (card 1) uses the pure conditional (N_rr⁻¹)_cc
                        vard = est.conditionalLocalVarianceDiag(
                            est_idx, {R.pl_zwd.col}, plan.shared_datum);
                } catch (...) { continue; }
                if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
                const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
                if (!std::isfinite(value_abs)) continue;
                double var;
                std::vector<CoupledShared> coupling;
                // ZWD in the local block (B9-in-block, network.carry_zwd_in_block): when
                // this station's pl harvest above put the ZWD column in `cols` (and only
                // then), the ZWD is harvested through the same cross-gather every
                // ambiguity column used (one call, one solve), and its row is written at
                // block index n = cols.size()−1.  zblk is null on every other
                // station-epoch, and off ⇒ nullptr/nullptr, the call as it is without the
                // knob (byte-identical).
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
                // Prior-residual census hook for the ZWD (INC-2): the ZWD carry has no
                // screen of any kind (no slip detector, no jump test, no D5 analogue),
                // which is the standing-invariant-#1 gap the census inventories.  Unlike
                // pl, `folded` is recorded directly and needs no reproduction: PASS B
                // records R.pl_zwd_injected at the injector.  The prior is read before
                // the stash below overwrites it.  Also gated on an inject-capable epoch
                // (B9 or a live seam window), so the capture-only path adds no rows and
                // the census of a run with a snapshot path stays byte-identical to one
                // without the capture.
                if (prires_zwd && prires_est.enabled() && (zwd_on || zwd_seam_live)) {
                    const auto zit = zwd_carry.find(static_cast<int>(i));
                    if (zit != zwd_carry.end()) {
                        const double zgap = pl_now_s - zit->second.last_seen_s;
                        if (zgap >= 0.0 && zgap <= pl_max_gap_s) {
                            NetworkPriorResidual::Input in;
                            in.family   = "zwd";
                            in.station  = static_cast<int>(i);
                            in.e        = zit->second.value_abs - value_abs;
                            // Mirror the injector's q exactly (RS-10b): inside a live
                            // seam window an armed network.zwd_seam_reseed_q applies
                            // there, and a census computed against a variance no row used
                            // would report the `zwd` family's σ̂₀ (the R1' instrument) off
                            // by the override ratio.
                            in.fold_var = zit->second.var
                                        + ctx.knobs.zwdQEff(zwd_seam_live)
                                              * std::max(0.0, zgap);
                            in.post_var = var;
                            in.gap_s    = zgap;
                            in.folded   = R.pl_zwd_injected;
                            // A ZWD carry within its horizon that did not inject is not
                            // a mechanism response (nothing fired on it); the injector had
                            // no live column or a non-positive var_eff.  CLEAN with the
                            // `+` branch is the correct classification, and the mech tag
                            // records it.
                            in.label = PriorResidualLabel::Clean;
                            in.mech  = R.pl_zwd_injected ? "" : "not_injected";
                            prires_est.addRow(in);
                        }
                    }
                }
                // Observatory, ZWD half (§18.12).  The Z rows are restored and counted
                // like the L rows (n_resume_cpl_zrows, and they arm the same
                // resume_cpl_census_pending latch), so a census that covered only the L
                // side would report rel ≈ 0 on a leg whose ZWD regression was stale, an
                // instrument that reads clean because it never looked (invariant #4).
                // Same measure-only body as the phase-leveling census above; read before
                // the stash below moves `coupling` out.
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
                networkSharedHistoryNoteEntry(shx_h, est_idx, R.pl_zwd.col, SharedHistoryMap::Zwd,
                                              static_cast<int>(i));   // card SCL2 X2; no-op off
                // ZWD in the local block: row n of the station's harvest block is the
                // ZWD column's own n_c read at every carried column (its last entry
                // equals `var`, the pure conditional).  The entry then points at the same
                // PlHarvestBlock as the ambiguity entries, so next epoch's inject pairs
                // ZWD↔ambiguity by pointer equality, as it pairs ambiguity↔ambiguity,
                // and with nothing stashed at a different harvest.  A ZWD whose harvest
                // failed above does not reach here: its entry keeps blk = nullptr, and
                // every pair with it is zeroed and counted (pl_blk_zwd_missing_cross),
                // which is the safe degrade.
                if (zblk && zcross.size() == zblk->C.rows()) {
                    const Eigen::Index zrow = zblk->C.rows() - 1;   // the appended column
                    zblk->C.row(zrow) = zcross.transpose();
                    auto& zent = zwd_carry[static_cast<int>(i)];
                    zent.blk     = zblk;
                    zent.blk_idx = static_cast<int>(zrow);
                }
            }
            // The horizon is the registry's (network.phase_leveling_max_gap_s, the same
            // double as pl_max_gap_s; card PRM phase 3b).
            carryGapPrune(zwd_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::Zwd).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_cr_zwd_gap_prune;   // same predicate, same census
                              prmRespond(ctx.counters.prm_resp, ResponseSite::ZwdGapPrune);   // card PRM phase 3a
                          });
            // Seam-window bookkeeping (network.zwd_seam_reseed): count this epoch's
            // actual folds (R.pl_zwd_injected is the injector's own record), advance the
            // retirement clock, and log the decoupling edge when the horizon is
            // exhausted (invariant #7; the ARMED line at the resume was the re-coupling
            // edge).  Serial section: the parallel produce phase only reads
            // zwd_seam_ep_left.
            if (zwd_seam_live) {
                long long seam_folds = 0;
                for (size_t i = 0; i < stations.size(); ++i)
                    if (est_index_of[i] >= 0 && asm_res[i].pl_zwd_injected) ++seam_folds;
                ctx.counters.n_zwd_seam_folds += seam_folds;
                if (seam_folds > 0) ++ctx.counters.n_zwd_seam_epochs;
                if (--ctx.carry.zwd_seam_ep_left == 0) {
                    // This seam has no logger parameter: resolve the logger per call and
                    // null-guard it (the spdlog free-function / cached-static hazard: do
                    // not cache it, do not dereference it unguarded).
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
        // Card SCL: every carried column of every station is harvested; add the clones'
        // within-station conditional covariance to the F3 blocks.  No-op off.
        networkClockCloneHarvestEnd(ccl_h, ctx);
        // Card SCL2 X2: with every carried row of the network harvested, compress the rows'
        // shared conditional term into the history and add each row's remainder (B1's end
        // above returns at once under X2).  No-op off.
        networkSharedHistoryHarvestEnd(shx_h, ccl_h, ctx, pl_now_s);
        // --- RPB carry (card 3, eq34): harvest δ̂_{r,σ}, stash and prune ----------
        // Mirrors the ZWD harvest.  There is no card-1 coupling harvest for RPB
        // columns, on purpose: coupling rows would give the RPB a de facto
        // shared-border footprint (the D6 v2 caveat zone, tuGaugeRestore group
        // exclusions); the diagonal conditional variance keeps the carry strictly
        // local and conservative.
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
            // The horizon is the registry's (network.phase_leveling_max_gap_s, the same
            // double as pl_max_gap_s; card PRM phase 3b).
            carryGapPrune(rpb_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::RxPhaseBias).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_cr_rpb_gap_prune;   // same predicate, same census
                              prmRespond(ctx.counters.prm_resp, ResponseSite::RpbGapPrune);   // card PRM phase 3a
                          });
        }
        // --- Slant-iono carry (queue 8b): harvest the posterior per-LOS iono, stash
        //     and prune.  Mirrors the RPB harvest (several columns, diagonal conditional
        //     variance, and no card-1 coupling harvest on purpose, for the rx-CB
        //     reason).  Stashes every epoch regardless of the inject scope, so the
        //     carried value tracks the solution continuously and the re-draw-event
        //     inject always has a fresh pre-event value.
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
                        // CS-GF authority (§6.2 successor): stash the level-independent
                        // carrier-smoothed GF-code estimate instead of the solve's
                        // (level-locked) iono.  A LOS whose authority is uninitialized
                        // this epoch keeps its old carry aging toward eviction, and no
                        // stale value is stashed.
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
                    // Prior-residual census, family "iono": the one instrument that can
                    // see a wrong carried iono value (T4 pools phase rows only; the
                    // prefit census is in the x_lin frame; phase is self-consistent with
                    // a level re-levelled against the wrong carry).  `folded` comes from
                    // the injector's record (iono_injected_sats).  The prior is read
                    // before the stash below overwrites it.
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
            // The horizon is the registry's: under the diagonal carry it is
            // network.iono_carry_max_gap_s, the same double as iono_max_gap_s (the loader
            // refuses network.carry_iono with network.carry_iono_in_block; card PRM
            // phase 3b).
            carryGapPrune(iono_carry, pl_now_s,
                          ctx.params.spec(NetworkParamKind::SlantIono).gap_horizon_s,
                          [&](const auto&) {
                              ++ctx.counters.n_iono_carry_evicted;
                              prmRespond(ctx.counters.prm_resp, ResponseSite::IonoGapPrune);   // card PRM phase 3a
                          });
        }
    }

    // --- Receiver-DCB datum gauge census (RXCB-DATUM) --------------------------
    // The zero-mean receiver-DCB datum's fold count, in its own walk, gated on its own
    // knob.  network.rx_code_bias_datum and network.carry_rx_code_bias are independent,
    // and the datum without the carry is the canonical posture; counting this inside
    // the carry bracket below would leave n_rxcb_gauges at 0 on every such run, and the
    // run-end RXCB-DATUM line would then raise its invariant #4 alarm against a working
    // gauge.
    //
    // This is the only accumulation site, so a run with both armed cannot double count.
    // It is also byte-identical on the carry-only posture: R.pl_cb_datum_rows is
    // incremented only under `rx_cb_datum` (the RXCB-DATUM row build in
    // ApplicationNetworkAssembly.cpp), so it is 0 whenever this walk is skipped.
    if (ctx.knobs.rx_cb_datum) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            ctx.counters.n_rxcb_gauges +=
                static_cast<size_t>(asm_res[i].pl_cb_datum_rows);
        }
    }

    // --- rx-CB carry: harvest, stash and prune -------------------------------
    // Outside the carry_warm bracket above on purpose: the receiver code-bias carry has
    // to be live on the carry_invariant lineage without phase leveling (the carry_warm
    // gate is what makes the ZWD carry inert there).  Mirrors the RPB harvest: several
    // columns per station, conditional local variance, LocalCarry stash keyed
    // (station, CB key).  The produce phase only reads the map (find, never insert or
    // erase) and counts its decisions in R, which are summed here serially, so the whole
    // cycle is race-free by construction.
    if (ctx.knobs.rx_cb_on) {
        auto& rx_cb_carry = ctx.carry.rx_cb_carry;
        // The horizon is kRxCbMaxGapS (filter/NetworkRunTypes.hpp; spec §3: housekeeping
        // horizon), the one definition the inject, this harvest and the registry read.
        // This epoch's invariant steps (card LV-4), collected during the station walk
        // and judged after it: the screen needs the whole population to build a robust,
        // peer-relative scale (rxCbStepScale).  Empty unless armed.
        struct Lv4Pending { int station; std::string key; double step; double formal_sigma; };
        std::vector<Lv4Pending> lv4_pending;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            ctx.carry.rx_cb_injected_total += R.pl_cb_injected;
            // n_rxcb_gauges is not summed here: it belongs to the RXCB-DATUM knob, not
            // the carry, and has its own walk above.  Every counter left in this bracket
            // is carry-only: their producer-side increments all sit below the
            // `if (!rx_cb_on) continue;` in the assembly's pl_cb walk, so a datum-only run
            // cannot populate them.
            ctx.carry.rx_cb_innov_skips_total += R.pl_cb_innov_skips;
            // The same two tallies, kept per station (RS-9 rider, §21.18).  The pooled
            // "N folded, M withheld" line cannot tell a share of stations refused
            // occasionally from a similar share refused always; in the second case the set
            // is fixed from k = 1, and those stations carry no receiver-DCB memory for the
            // whole run while the line still counts them among the "N carried".  The
            // denominator is station-epochs with a decision; the numerator is
            // station-epochs in which every decision was a withhold ("carried nothing",
            // the multi-family generalization of the one-key field posture).  Serial, so
            // no race.
            if (R.pl_cb_injected > 0 || R.pl_cb_innov_skips > 0) {
                auto& tally = ctx.carry.rx_cb_sta_tally[stations[i].name];
                ++tally.first;
                if (R.pl_cb_injected == 0) ++tally.second;
            }
            ctx.carry.lv2_folds_demeaned += R.pl_cb_demeaned;   // de-meaned folds (card LV-2)
            // Same-run A/B for card LV-4, summed with the other produce-phase tallies
            // before any early continue, so a station whose back-substitution throws
            // still reports what the retired screen would have done to its folds.
            ctx.carry.lv4_frame_gate_would_skip += R.pl_cb_frame_gate_would_skip;
            // RXCB-IN-BLOCK: the stash happened in the block harvest above (with the
            // border regression), and only while the leveling carry is warm; a cold
            // entry must not be written here without one.
            if (ctx.knobs.rxcb_blk) continue;
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
                // Prior-residual census hook (INC-2).  `folded` is re-derived by calling
                // rxCbInjectGate with the same arguments PASS B used: the gate is a pure
                // function, and every input is still in hand and unchanged at this point
                // (the stash below overwrites the entry), so this reproduces the
                // injector's own decision exactly rather than mirroring it.  The
                // SkipInnov branch is an invariant #1 event response (receiver reset or
                // firmware bias jump: withhold the fold, let the harvest re-seed), so it
                // is the RESPONDED positive control for this family.
                if (prires_rxcb && prires_est.enabled()) {
                    const auto pit = rx_cb_carry.find({static_cast<int>(i), cb_key});
                    if (pit != rx_cb_carry.end()) {
                        const double cgap = pl_now_s - pit->second.last_seen_s;
                        const double cinnov = pit->second.value_abs - cx_lin;
                        double cvar_eff = 0.0;
                        const RxCbInjectAction cact = rxCbInjectGate(
                            cgap, kRxCbMaxGapS, pit->second.var, ctx.knobs.rx_cb_q,
                            cinnov, /*k_sigma=*/10.0, /*guard_var=*/0.25, cvar_eff,
                            // Mirror the injector's configuration, or the census
                            // would label rows "responded" for a test the run does
                            // not apply.
                            /*innov_test=*/!ctx.knobs.rx_cb_step_gate);
                        // SkipGap leaves cvar_eff at 0 (the gate returns before setting
                        // it), so there is no fold variance to standardize against: fall
                        // back to the nominal recompute and mark the row as raw formal
                        // (var_nominal) rather than invent a scale.
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
                // --- Frame-invariant event screen (card LV-4) ---------------------
                // docs/SPEC_level_information_budget.md.  This is the only place in the
                // cycle where two absolute realizations of the same receiver bias exist
                // side by side, the freshly back-substituted posterior (cx_lin + xr) and
                // the carried one, so their difference is the one discrepancy that no
                // reframing, reseeding or datum change can move.  The retired inject
                // test compared the carried value against a frozen x_lin instead, so it
                // withheld folds of stations that had no event and missed the jumps it
                // was meant to detect (at inject time the carried value is still the
                // pre-jump one).
                //
                // The decision cannot be taken here: it needs this epoch's whole
                // population (see rxCbStepScale: a formal-σ test triggers en masse, and a
                // step common to every station is a frame move, not a set of receiver
                // resets).  So the step is recorded here and the screen runs after this
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
        // --- Decision pass of card LV-4 (serial, all stations) -----------------
        // Robust and peer-relative, per code-bias family: σ̂ = 1.4826·MAD of this epoch's
        // steps (floored at the median formal σ), and the median step is removed first,
        // because a step common to the network is a frame move, not an event.  What is
        // left is the per-station outlier.  The response is an erase, not an overwrite
        // (reset, not repair): a real jump means this epoch's solve folded a stale
        // pre-jump prior, so the posterior it produced is suspect too.  The entry goes
        // cold for one epoch and the next harvest re-seeds it from data, the same
        // lifecycle as a first sighting.
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
                const std::size_t prm_n_cb = rx_cb_carry.erase({p.station, p.key});
                ++ctx.carry.lv4_step_resets;
                // Card PRM phase 3a: the carried receiver code bias the step gate erased.
                prmRespond(ctx.counters.prm_resp, ResponseSite::RxcbPeerStepErase,
                           static_cast<long long>(prm_n_cb));
            }
        }
        // The horizon is the registry's (kRxCbMaxGapS, the same constant; card PRM phase 3b).
        carryGapPrune(rx_cb_carry, pl_now_s,
                      ctx.params.spec(NetworkParamKind::RxCodeBias).gap_horizon_s,
                      [&](const auto&) {
                          prmRespond(ctx.counters.prm_resp, ResponseSite::RxcbGapPrune);   // card PRM phase 3a
                      });
        // --- S-consistent harvest (card S2H; serial, all stations) --------------
        // docs/SPEC_level_information_budget.md §17 item 3 / §27 item 1.  Runs after the
        // LV-4 erase pass and the gap prune, so the member set is exactly the set that
        // can fold next epoch, and the S-transform re-realizes the carried
        // representation that will exert force.  Per family (the CB state key, with
        // constellation and DCB/IFB kept apart, the LV-2 rule), subtract the Λ-weighted
        // mean of the harvested absolute realizations from every entry, fresh and
        // stale-but-alive alike: the whole carried set moves to the zero-mean frame
        // together, so next epoch's folds exert zero net force on the common mode in the
        // fold's own metric.  The datum row (whose target is the same absolute zero) owns
        // the level, and the frame is re-derived from this epoch's solution.  There is no
        // frozen pin (the LV-2 failure: it pinned mean(x_lin)), no lag-1 feedback
        // (nothing is fed back) and no cross-station prior row (the carry stays
        // diagonal).  This is LASER
        // Net3DLsa::applySTransformationForBigSystemOfEquations, Λ-weighted; see
        // rxCbHarvestMu for the two documented deviations.
        //
        // The covariance is co-transformed exactly (diag of S·Q·Sᵀ for diagonal Q):
        // voters get var − 1/Σλ (their uncertainty about the new frame is smaller, since
        // the common-mode variance left with μ), and screened non-voters get var + 1/Σλ
        // (μ's own variance adds, by independence).  The floor guards the w→1 degenerate
        // corner where one member is the frame; with min_n ≥ 2 and comparable weights it
        // does not bind (1/Σλ ≈ var/n).
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
        // --- Common-mode mean of card LV-2, computed here (serial) --------------
        // docs/SPEC_level_information_budget.md.  The parallel produce phase
        // accumulated, per station and per constellation, Σλ·innov and Σλ over the rows
        // it folded (the gate's skips contribute nothing: a withheld row exerts no force,
        // so it does not vote on the force being removed).  Reducing them here and
        // applying the result with a one-epoch lag keeps the cycle race-free by
        // construction, like the carry itself; but the lag is a feedback delay, so under
        // card LV-5 μ is reduced before PASS B instead (networkRxCbDemeanPrepass) and
        // this pass becomes a diagnostic (see the branch below).
        //
        // μ_c = Σλ·innov / Σλ  ⇒  Σλ·(innov − μ_c) = 0 in delta coordinates: the carry
        // keeps every between-station difference at full stiffness and stops
        // re-asserting the previous epoch's common-mode realization (the flywheel).
        //
        // It does not free that direction (SPEC §13).  The de-mean moves the RHS, not the
        // fold's information, so Σλ ≈ 100 units still land on the common mode, and in
        // absolute terms each row targets value_abs−μ, whose weighted mean is
        // mean(x_lin).  The de-meaned carry therefore pins the receiver-DCB common mode
        // to the frozen first-epoch code-bias seed frame, replacing a rolling memory with
        // a frozen one.  That makes it close to the rejected "demeaned target" variant;
        // the difference is that the pinned value is epoch 1's data-derived mean rather
        // than the physical zero, which is why card LV-1 (which sets that seed through
        // initializeCodeBiases) moves the level by about 10 TECU with no change to the
        // row set.
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
            // Invariant #7 (both edges): a decoupling epoch (mass re-seed, RAIM
            // re-reduce, a thin station set) does not define the frame.  Below the floor
            // μ freezes at its previous value (a held frame is conservative: it asserts
            // nothing new), and the event is counted, so a run that stays frozen through
            // an era reports it.
            // With the within-epoch reduction of card LV-5 armed, this pass is a
            // diagnostic only.  μ was already formed between PASS A and PASS B and folded
            // against; the folded set's raw mean now measures the residual common-mode
            // force that survived the correction, the direct check that the de-mean
            // worked (it is not identically zero, because the folded set is not exactly
            // the candidate set the prepass averaged).  Overwriting μ here would
            // reinstate the lag without any sign in the output.
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

    // --- One-epoch-lag snapshot of the solved SH-VTEC field (card LV-1) ---------
    // docs/SPEC_level_information_budget.md.  Copied serially here (post-solve, x_s
    // final) and read next epoch by the parallel produce phase, which samples it at
    // each new LOS's pierce point, so a per-LOS iono state is born in the field's frame
    // instead of the frame with the receiver bias included that the raw geometry-free
    // code seed carries.  Coefficients only: the pierce geometry and the sun-fixed
    // rotation belong to the epoch that evaluates them, not to the epoch that solved
    // them.  Cheap ((N+1)² doubles per shell, 81 at degree 8) and untouched when the
    // card is disarmed.
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
        // A partial or failed read leaves the previous snapshot in place (or none on
        // epoch 1, which falls back to the a₀₀ climatology prior mean, the designed
        // level authority).  A half-built field is never installed.
        if (ok) {
            ctx.carry.sh_field_lag1 = std::move(snap);
            ++ctx.carry.lv1_field_snapshots;
        }
    }

    // Retire the first-warm-epoch latch at the end of this epoch's serial harvest
    // (§18.12).  Everything the latch gates (the C1 diagonal-border count in
    // networkInjectD2CarryPrior and the present/departed fold split, both earlier in the
    // epoch, and the restored-against-fresh regressor census above) therefore covers
    // exactly the first
    // post-resume epoch.  Unconditional: a run that did not resume leaves it false, and
    // clearing a false bool is a no-op on every off path.
    if (ctx.carry.resume_cpl_census_pending)
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
    ctx.carry.resume_cpl_census_pending = false;
}

void networkRetroUraBookkeeping(NetworkRunContext& ctx,
                                const std::set<std::string>& clock_refloat_sats,
                                const std::set<std::string>& retro_flag_sats,
                                bool retro_feed) {
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& retro_active = ctx.carry.retro_active;
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate;
    const int    pl_warmup = ctx.knobs.pl_warmup;
    const bool   ura_retro = ctx.knobs.ura_retro;

    // After all post-emit QC tiers, record the flagged satellites for the next emit's
    // retro-bump (per-satellite disc bump) and post-refloat URA inflation horizon (B1).
    // The flag set is the union of clock_refloat_sats (clock model / clock monitor, plus
    // the coupled RAIM tiers) and retro_flag_sats (the tier-independent P0-2
    // accumulator), so the retro-bump covers every QC tier regardless of the
    // clock_monitor_couple_raim opt-in.
    if (retro_feed) {
        std::set<std::string> flagged_now;
        for (const std::string& sat : clock_refloat_sats)
            if (!sat.empty()) flagged_now.insert(sat);
        for (const std::string& sat : retro_flag_sats)
            if (!sat.empty()) flagged_now.insert(sat);

        if (ura_refloat_inflate > 1.0 && pl_warmup > 0)
            for (const std::string& sat : flagged_now) ura_refloat_horizon[sat] = pl_warmup;

        // The retro-bump is edge-triggered (P1-3): queue only a satellite that just
        // became flagged (not already in retro_active), so a satellite that stays
        // flagged through several recovery epochs is reset once at the consumer, not at
        // every emit.  A satellite that clears drops out of retro_active, so a later
        // re-flag re-arms the bump.  ura_retro_sats is consumed at the next emit.
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
    // Card PRM phase 2: the stash re-expresses the carry in the solve's datum, so a transform
    // the handover preview applied before PASS B is no longer a pending record (empty with
    // network.handover_stransform_before_rows off).
    ctx.carry.sts_carry_pivot.clear();
    // Alias block (INC-4): the relocated body reads the former runNetwork() locals under
    // their original names.
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

    // --- Stash of the D2 carry for the next epoch ------------------------
    // READMIT (E2): keep the outgoing stash, so departures can be parked after the
    // rebuild.
    const double readmit_h = ctx.knobs.readmit_seed_horizon_s;
    std::vector<CarryPrior> old_carry;
    if (readmit_h > 0.0) old_carry = prev_carry;
    prev_carry.clear();
    prev_carry_cov.clear();   // rebuilt below when the dense carry (C1) is on
    std::vector<Eigen::Index> fc_cols;  // shared-border column per dense row (C1)
    // Fix A: covariance-carry consistency, the companion to the point-solve fold that
    // the solve depends on.  Under the anchor-free ensemble phase-bias datum the gauge
    // is realized after the reduction by the rank-1 correction Ñ_ss += g gᵀ/β
    // (@ref NetworkNormalEquations::foldReducedSumZeroGauges), not by a local pin before
    // the reduction as in the anchor mode.  A satellite phase-bias column is the gauge
    // (null) direction of the unfolded reduced normals: the fold makes the system
    // invertible but leaves that column's slot of the reduced covariance carrying the
    // fold's artificial gauge-direction variance (an over-tight value that reinforces
    // itself epoch by epoch, set by the sum-to-zero reference rather than by a physical
    // uncertainty, since the ensemble reference is re-realized exactly every epoch),
    // plus its phase↔clock / phase↔code cross terms.  Feeding that column into the D2
    // carry would propagate the fold into next epoch's prior: the stiff,
    // gauge-referenced phase-bias prior biases the coupled satellite clock through
    // N_ss[clk,pb], clock_model amplifies it, and the pair diverges exponentially from
    // epoch 2 (about 7× per epoch).  Therefore every ensemble phase-bias gauge column
    // is treated as an exact datum (variance 0) in the covariance the carry harvests,
    // the analogue of how conditionalLocalCovariance / conditionalLocalVarianceDiag
    // zero the datum-pinned shared columns before reconstructing a conditional
    // covariance ("datum-pinned rows/cols zeroed → treat as exact").  Zeroing the row
    // and column drops both the over-tight diagonal (the diagonal stash's var>0 guard
    // then skips the key) and the phase↔clock/code cross-covariance (the key is then
    // excluded from the C1 dense block too), while the gauge-invariant clock and
    // code-bias variances (which the S-transform leaves identical to the anchor datum)
    // pass through untouched.  Continuity of the emitted phase bias comes from the
    // local ambiguity carry (phase_leveling) and the stable ensemble constraint, not
    // from this shared carry, which under the fold would double-count that information
    // and add the contamination.  Anchor mode or off: cov aliases
    // est.sharedCovariance() unchanged → byte-identical.  Zeroing the g direction
    // instead (g = λ·N_ss·e_pb, which has clock components) would corrupt the correct
    // clock covariance: the contaminated direction is the phase-bias column e_pb, not g.
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
        if (fullcov_on) {   // register this entry's dense row, same skip rules (C1)
            prev_carry_cov.row.emplace(sharedCarryKey(kind, sat, signal),
                                       fc_cols.size());
            fc_cols.push_back(col);
        }
    };
    // A satellite flagged by the clock monitor (or, opt-in, by the within-epoch / post-fit
    // RAIM) has its carried shared state reinitialized rather than frozen.  Two modes: with
    // clk_mon_reinit_sig > 0 (default) it is still stashed but marked inflate_q, so next
    // epoch folds it with the loose reinit variance (a weak anchor that lets a frozen clock
    // move back toward truth while limiting the datum drift a hard refloat causes); with
    // clk_mon_reinit_sig ≤ 0 it is withheld entirely (hard skip-stash refloat, no prior).
    // clock_refloat_sats is empty unless network.clock_monitor is on, so the stash is
    // byte-identical when the monitor is off (nothing flagged → inflate=false → the
    // ordinary stiff-carry stash).
    const bool full_refloat = !(clk_mon_reinit_sig > 0.0);   // reinit_sigma ≤ 0 → hard skip-stash
    // Carry this stash epoch's clock-monitor flags across to the next inject (hard-refloat
    // mode skip-stashes the flagged satellite entirely, so the flag cannot travel on
    // prev_carry.inflate_q).  Gated, so off runs are unchanged.  Card 1g reads the same
    // history with κ off, so the mirror follows the wider predicate; otherwise arming 1g
    // alone would leave the clock-QC half of the feed empty, and the widening would cover
    // only the reref sources without any indication.
    if (carry_cpl_tu || ctx.knobs.carry_cpl_reinit)
        prev_clock_refloat_sats = clock_refloat_sats;
    // The satellite clock is stashed to prev_carry only in scalar-carry mode.  Under
    // network.clock_model the clock is carried by clock_osc and is not stashed here; the
    // two-state model is its only cross-epoch memory.
    if (!clk_model_on)
        for (const std::string& sat : registered_clock_sats) {
            const bool flagged = clock_refloat_sats.count(sat) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
        }
    // eq34 reattach hygiene (INC-2): refresh the bridge's value source from this kept
    // epoch's solved shared phase biases.  Rebuilt wholesale in lock-step with the D2
    // carry (a departed key drops out, so a bridge cannot be opened on a level from before
    // an absence), and harvested here, at the stash, which every whole-epoch withhold path
    // skips with `continue`, so a withheld epoch's transient solve never becomes a held
    // level.  Not read from prev_carry, on purpose: under carry_invariant the shared
    // PhaseBias carry is not stashed at all, and that is the configuration the bridge
    // targets.  The S-basis completion (network.sbasis_complete, SBC) reuses this harvest
    // for the same reason: its continuity targets need the last solved phase-bias levels
    // on the carry_invariant configurations.  Empty and untouched when neither consumer
    // is armed.
    const bool harvest_pb_levels =
        ctx.knobs.pin_bridge_on || ctx.knobs.sbasis_complete_on;
    if (harvest_pb_levels) ctx.carry.pb_level_prev.clear();
    for (const auto& pk : shared_phase_keys) {
        const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
        if (!parts) continue;
        // Datum-station slip re-reference (§9.7.10,
        // network.cycle_slip.datum_station_detect): a slip at the gauge-pinned datum
        // receiver moved the level that every phase bias of this satellite references.
        // Skip the stash entirely for one kept epoch, before the pb_level_prev harvest (a
        // bridge must not re-open on the stale level) and before both stash branches
        // (plain and phase_hold), so the next solve re-seeds the bias at the new pivot
        // level, in lock-step with the network-wide ambiguity reseed that the slip drain
        // defers until this skip has been consumed.  The set is empty unless the toggle
        // is armed ⇒ byte-identical.
        if (ctx.carry.datum_reref_pb_sats.count(parts->satellite)) continue;
        if (harvest_pb_levels) {
            // Guarded: this runs before the carry_invariant skip below, so it is the one
            // site that queries a phase-bias column for every shared key on a path that
            // otherwise would not.  A key without a column is not harvested (a bridge
            // then cannot open on it); the stash does not throw.
            try {
                const double v =
                    x_s(est.phaseBiasColumn(parts->satellite, parts->signal));
                if (std::isfinite(v))
                    ctx.carry.pb_level_prev[StateKeys::makeAmbiguityKey(parts->satellite,
                                                                        parts->signal)] = v;
            } catch (...) {}
        }
        // The DD invariant carry is the phase time-link, so the S-dependent shared
        // PhaseBias carry is not stashed as well (it would double-count in next epoch's
        // D2 / hold / dense fold).  Clock / code / SH / ZWD carries are unaffected.
        if (carry_invariant) continue;
        const bool flagged = clock_refloat_sats.count(parts->satellite) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col =
            est.phaseBiasColumn(parts->satellite, parts->signal);
        if (phase_hold_on) {
            // Seed once, then time-lock: carry b_s toward its realized value with the
            // fixed hold variance, the temporal datum that stops the metre-per-epoch
            // re-realization.  Not cov(col,col), on purpose: the ensemble fold leaves that
            // column carrying an artificial gauge variance (carrying it causes the epoch-2
            // runaway that the Fix A family guards against).  A stiff, hand-set variance
            // makes the carried value the datum reference next epoch (buildEnsembleGauges
            // then skips the Σz=0 fold for it).  Datum-pinned columns (none for PB) and
            // non-finite values are still excluded.
            if (pinned_cols.count(col)) continue;
            const double v = x_s(col);
            if (!std::isfinite(v)) continue;
            CarryPrior cp;
            cp.kind = SharedParamKind::PhaseBias;
            cp.sat = parts->satellite; cp.signal = parts->signal;
            // inflate_q is always false: a clock-monitor soft refloat of this satellite
            // (the loose-reinit path the legacy stash applies) does not loosen the
            // phase-bias hold, on purpose, because the phase-ensemble datum is orthogonal
            // to a clock fault, so the bias stays time-locked.  (A hard refloat is still
            // handled: `flagged && full_refloat` skip-stashes above, so the satellite
            // re-seeds next epoch.)
            cp.value = v; cp.var = phase_hold_var; cp.inflate_q = false;
            prev_carry.push_back(cp);
        } else {
            stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal,
                  col, flagged);
        }
    }
    // The datum re-reference skip-stash queue (§9.7.10) is consumed exactly once per
    // kept epoch (a withheld epoch never reaches this stash, so the queue, and the
    // deferred ambiguity reseed keyed on its consumption, survive the withhold intact).
    // Cleared wholesale: a queued satellite with no phase-bias key this epoch had nothing
    // stashed to skip, and prev_carry is rebuilt from scratch above, so no stale carry is
    // left to guard against.
    if (!ctx.carry.datum_reref_pb_sats.empty()) ctx.carry.datum_reref_pb_sats.clear();
    // Refresh the time-linked previous-value map from this kept epoch's solved code
    // biases.  Cleared and rebuilt (a departed satellite drops out, so its return
    // re-seeds that satellite alone) in lock-step with the D2 carry, with the same
    // disposition rules (a hard-refloated satellite is skipped from both, so it re-seeds
    // next epoch, with its re-reference signalled as a disc).  Written to the snapshot
    // only under network.state_snapshot_datum_capture (RS-3).  Empty and untouched when
    // off.
    if (cb_time_linked) {
        code_bias_prev_.clear();
        // Advance the IF-pin comparison baseline to this kept epoch's pairs (a pair
        // recorded on an epoch that was then withheld never reaches here, so the
        // baseline tracks emitted / kept datum realizations only).
        code_bias_ifpin_pair_ = this_epoch_cb_ifpin;
    }
    for (const auto& kv : registered_code_keys) {  // satellite code bias, OSB (W6)
        const bool flagged = clock_refloat_sats.count(kv.first) > 0;
        if (flagged && full_refloat) continue;
        const Eigen::Index col = est.codeBiasColumn(kv.first, kv.second);
        // CB-excursion guard (SPEC_day129_frame_guards phase 2.1): a network-wide
        // common-mode clock excursion this epoch marks the affected satellite's code-bias
        // carry for reinit, and only the code-bias carry.  The clock and phase-bias
        // carries are left untouched on purpose: the clock is soft (q = 0.3 m²/ep; it
        // recovers on its own in 2–4 epochs), and the phase biases did not move
        // measurably (≤ 4 mm at the DOY-129 event).  The near-static code bias is the
        // state that ratchets, so it is the state the guard resets.  The set is empty
        // unless network.cb_excursion_reinit is on ⇒ byte-identical when off.
        const bool cb_excursion = ctx.carry.cb_excursion_sats.count(kv.first) > 0;
        stash(SharedParamKind::CodeBias, kv.first, kv.second, col,
              flagged || cb_excursion);
        if (cb_time_linked) {
            const double v = x_s(col);
            if (std::isfinite(v)) {
                code_bias_prev_[constellationKey(systemOfSat(kv.first)) + "|" + kv.second]
                               [kv.first] = v;
                code_bias_sat_seen_.insert(kv.first);
            }
        }
    }
    // Dense carry (C1): stash the dense covariance block over the clock/phase/code
    // entries just recorded (the SH block below keeps its own diagonal stash; its carry
    // is the separate fold before the gate).  One O(k²) copy from the solve's
    // posterior.  A single non-finite entry clears the whole block, and the next fold
    // degrades per key to the diagonal path instead of corrupting the next epoch's
    // normals (the crash mode the SH guard below screens per entry).
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
    // Card CTU: stamp the block with this epoch's content time (a coupling row is exact
    // only if it was harvested now, against this same posterior) and record which clock
    // the datum pinned (the pivot takes each exact row's clock-family deficit next epoch).
    if (ctx.knobs.carry_cpl_exact) {
        ctx.carry.cxu_stash_s     = TimeUtils::continuousGpsSeconds(t);
        ctx.carry.cxu_stash_valid = prev_carry_cov.P.rows() > 0;
        ctx.carry.cxu_pivot.clear();
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            Eigen::Index col = -1;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (pinned_cols.count(col) && col >= 0 && col < x_s.size())
                ctx.carry.cxu_pivot[letterOfSat(sat)] = {sat, x_s(col)};
        }
    }
    // Card PFQ: the clock pivot this stash was pinned on, the frame of the carried clocks
    // the next epoch's fold applies the pivot-frame process noise in.
    if (ctx.knobs.pfq_on) {
        ctx.carry.pfq_pivot.clear();
        for (const std::string& sat : registered_clock_sats) {
            if (sat.empty()) continue;
            Eigen::Index col = -1;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            if (col >= 0 && pinned_cols.count(col)) ctx.carry.pfq_pivot[letterOfSat(sat)] = sat;
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
                // A non-finite value or variance is not carried forward: it would
                // corrupt the next epoch's normals and crash the solve.  The
                // mean-reverting climatology prior re-seeds the coefficient.
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

    // ---- READMIT continuity (E2 seed-first re-admission and the block re-coupling edge) ----
    // (SPEC_synthetic_minimum_network §18.4; SPEC_amb_iono_split_overcount §7 FIRST.)  Runs
    // after the wholesale rebuild above, so "absent from the new stash" is exact, and on
    // kept epochs only: a withheld epoch never reaches here, which is why the detector
    // needs >= kReadmitMinGapEp loop epochs of absence rather than one.
    if (readmit_h > 0.0 || ctx.knobs.readmit_marg_ep > 0) {
        const long long ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        // (1) The re-admission detector and the coupling marginalization window (the
        //     block's decoupling edge, standing invariant #7): a satellite whose clock
        //     column is registered now but was last stashed >= kReadmitMinGapEp loop
        //     epochs ago.
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
        // Expired windows: the block's re-coupling edge (the returned satellite's
        // columns fold as present terms again), traced per satellite.
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
                // a stiff PB seed would conflict with that pin, so only Clock and CodeBias
                // park.
                if (oc.kind != SharedParamKind::Clock
                    && !(oc.kind == SharedParamKind::CodeBias && ctx.knobs.readmit_seed_cb))
                    continue;
                if (live.count({static_cast<int>(oc.kind), oc.sat, oc.signal})) continue;
                const bool present = carryColumnPresent(est, oc);
                if (oc.t_park_s > 0.0) {
                    // Already parked.  Column back but not re-stashed (skip-stash,
                    // pinned, non-finite) ⇒ the memory is dropped with the skip; past the
                    // horizon ⇒ drop.
                    // Card PRM phase 3a: each drop counted per kind of the parked entry
                    // (a Clock, or a CodeBias under readmit_seed_cb: the filter above).
                    if (present) {
                        ++ctx.counters.n_readmit_dropped_stale;
                        prmRespond(ctx.counters.prm_resp,
                                   oc.kind == SharedParamKind::Clock
                                       ? ResponseSite::ClkParkDiscard
                                       : ResponseSite::CbParkDiscard);
                        continue;
                    }
                    if (now_s - oc.t_park_s > readmit_h) {
                        ++ctx.counters.n_readmit_expired;
                        prmRespond(ctx.counters.prm_resp,
                                   oc.kind == SharedParamKind::Clock
                                       ? ResponseSite::ClkParkExpire
                                       : ResponseSite::CbParkExpire);
                        continue;
                    }
                    prev_carry.push_back(oc);
                    continue;
                }
                // Live last epoch, absent now: a departure only if the column itself
                // left the layout (a skip-stash / pinned / non-finite key is not a
                // departure).
                if (present) continue;
                CarryPrior cp = oc;
                cp.t_park_s  = (ctx.carry.readmit_prev_stash_s > 0.0)
                                   ? ctx.carry.readmit_prev_stash_s : now_s;
                cp.inflate_q = false;
                prev_carry.push_back(std::move(cp));
                ++ctx.counters.n_readmit_parked;
                prmRespond(ctx.counters.prm_resp,   // card PRM phase 3a, per kind
                           oc.kind == SharedParamKind::Clock ? ResponseSite::ClkPark
                                                             : ResponseSite::CbPark);
                ++parked_now[oc.sat];
            }
            for (const auto& [sat, n] : parked_now) {
                traces.emitEvent(t, "READMIT", "park", sat, "", static_cast<double>(n),
                                 readmit_h, "park",
                                 "D2 clock + code-bias carry parked at departure");
                // The READMIT writer (card SID phase 4a, S1).  The park is a shared
                // parameter's departure, so it names a satellite and never a station:
                // the expansion writes the bit on every station's arcs of that satellite
                // that the admitted lifecycle still considers alive.  This seam runs
                // after the roster's audit pass, so the record reaches the fold one epoch
                // later through @ref kArcReasonLateMask, the earliest epoch at which a gap
                // it can explain can open (the satellite has already departed).
                arcReasonExpand(ctx.reasons, ctx.arc_ledger, -1, sat, "",
                                kArcReasonReadmit);
            }
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
