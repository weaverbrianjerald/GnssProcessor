/**
 * @file NetworkKnobs.cpp
 * @brief @internal NetworkKnobs::snapshot: the run's configuration, read once into the
 *        const knob table of the network run context.
 *
 * One function, called by the NetworkRunContext constructor
 * (include/app/NetworkRunContext.hpp) and directly by unit tests.  Many assignments
 * combine knobs assigned above them, so the order of the lines matters.
 *
 * One of the seven files that define the network run context's functions: card NOM3 step
 * HV0 split them out of the former app/NetworkRunContext.cpp by concern, each body
 * unchanged.  The declarations, each naming its file, and the map of the seven files are
 * in include/app/NetworkRunContext.hpp.
 */

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // parseDataClassMask (the prefit census class masks)

#include "config/GnssConfig.hpp"
#include "filter/NetworkCarryVce.hpp"       // NetworkCarryVce::parseComponents (the prior-residual families)

#include <algorithm>

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
    k.nbp_on       = config_.networkAprioriBrdcClock();        // card NBP (fingerprinted in NetworkSnapshotFingerprint.cpp)
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
    // I-in-block; loader guarantees phase_leveling + pl_local_block + carry_cpl_cond and
    // !carry_invariant.
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
    k.rx_cb_on = config_.networkCarryRxCodeBias();   // loader guarantees code_datum_time_linked
    k.rxcb_blk = config_.networkCarryRxCodeBiasInBlock();   // RXCB-IN-BLOCK; loader guarantees rx_cb_on + coupling + a local block
    k.rx_cb_datum = config_.networkRxCodeBiasDatum();
    k.rx_cb_datum_sigma = config_.networkRxCodeBiasDatumSigmaM();
    k.sta_prior_pre_pd = config_.getNetworkConfig().station_prior_before_pd;
    k.rx_cb_q = config_.networkRxCbCarryQ();
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
    // the driver configures it next to the emit re-base setup.
    k.clk_frame_hold = config_.networkEmitClockFrameHold();
    k.clk_jump_test  = config_.networkClockDiscJumpTest();
    // Emit-side phase-bias hold (card PBH).  The held values and generations live in
    // NetworkCarryState because they outlive the epoch; these are the knobs the
    // NetworkPbHold seam reads.
    k.emit_pb_hold           = config_.getNetworkConfig().emit_pb_hold;
    k.emit_pb_hold_max_gap_s = config_.getNetworkConfig().emit_pb_hold_max_gap_s;
    k.emit_pb_hold_phase_clock = config_.getNetworkConfig().emit_pb_hold_phase_clock;   // card LAH
    k.emit_pb_hold_smoothed    = config_.getNetworkConfig().emit_pb_hold_smoothed;      // card SMH
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
    k.start_offset_s = config_.networkStartOffsetHours() * 3600.0;
    k.trace_flag_rates = nc.trace_flag_rates;
    k.phase_datum_eq34 = config_.networkPhaseDatumEq34();
    k.carry_invariant = k.phase_datum_eq34 && config_.networkCarryInvariant();
    k.carry_invariant_pd_safe = k.carry_invariant && config_.networkCarryInvariantPdSafe();
    k.carry_aware_covariance = k.carry_invariant && config_.networkCarryAwareCovariance();
    // Card ARL: the search's suspect-corner mask has nothing to mask without the search.
    k.ar_suspect_epochs = nc.ambiguity_resolution.enable
                              ? std::max(0, nc.ambiguity_resolution.suspect_epochs) : 0;
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
    // Card ICF response 1, gated on the screen and refused under the hold (the hold would
    // keep the faulted level the cold response erases).  Not fingerprinted, like the
    // screen's other knobs.
    k.gl_cold_bound_m  = (k.gl_on && !k.gl_bound) ? config_.networkGrossLosColdBoundM() : 0.0;
    k.raim_ci_refloat = k.raim_ci_on && config_.networkRaimCarryInvariantRefloat();
    // Card RPA: the RAIM pools restricted to observation phase rows.  ANDed with raim_on
    // (the loader already guarantees it), so both pool walks and the run-end lines read
    // one bool.  Not appended to the snapshot fingerprint (NetworkSnapshotFingerprint.cpp): a QC knob, like raim_*.
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
    k.phase_datum_prioritized = config_.networkPhaseDatumPrioritized() || k.phase_datum_eq34;
    // Resolved once for the run (§9.7.10): the datum-station slip path exists only under
    // the legacy pivot datum.  Every no-pivot datum already runs the slip loops from
    // i_lo = 0, so station 0 has ordinary coverage there.
    k.datum_station_detect = config_.networkCycleSlipDatumStationDetect()
                             && !k.phase_datum_prioritized;
    // eq34 reattach hygiene.  The loader already disarms every increment outside an
    // anchored election; `anchored` here is a second guard, so a hand-built
    // NetworkConfig (tests) cannot arm the seam on a datum that elects no anchor.
    {
        const bool anchored = k.phase_datum_prioritized;
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
    // The SH-deviation carry (card T5, Iono-2): live when a co-estimated SH field
    // (network.estimate_iono_sh) has a positive correlation time.  The epoch driver
    // tested the SH model (setup.sh_model) where this reads the flag: setup builds
    // the model exactly when the flag is set, and a run whose setup fails never reaches the
    // epoch loop or the run end (card NOM3 step EP0).
    {
        const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
        k.sh_dev_tau     = config_.estimateNetworkIonoSh() ? shc.rep_error_tau_s : 0.0;
        k.sh_dev_on      = k.sh_dev_tau > 0.0;
        k.sh_dev_demean  = k.sh_dev_on && shc.rep_error_demean;    // anti-ratchet de-mean (Iono-2 v3)
        k.sh_dev_d5_keep = k.sh_dev_on && shc.rep_error_d5_keep;   // erase-policy split (v3k)
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
                else if (f == "rxcb") k.prires_rxcb = true;
                else if (f == "clk")  k.prires_clk  = true;   // clock anchor (card Q3, INC-0)
            }
            k.prires_on = k.prires_pl || k.prires_pb || k.prires_cb
                       || k.prires_zwd || k.prires_rxcb || k.prires_clk;
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
    // --- Invariant-carry gate and emit layers (card NOM step TD1a) ---
    // Moved from runNetwork with their conjunctions unchanged.
    {
        // Estimation-side fine-slip innovation gate: the invariant-carry-native replacement
        // for the RAIM fine-slip layer that the invariant bypass empties.  Requires
        // carry_invariant (rejected at config load otherwise).
        k.carry_innov_gate = k.carry_invariant && config_.networkCarryInvariantInnovGate();
        // Emit-layer frame re-base (card 4 phase D), on by default when carry_invariant is
        // armed (spec D7).
        k.emit_rebase_on = k.carry_invariant && config_.networkCarryInvariantEmitRebase();
    }
    // --- The epoch driver's configuration-derived values (card NOM3 step EP0) ---
    // Moved from runNetwork with each expression unchanged (the SH-deviation switches are
    // resolved above).  The rest is read from `nc`, which is the driver's own
    // configuration in a run.
    {
        k.sec_timers_on = nc.trace_section_timers;
        k.clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();
        const std::vector<std::string> vce_families =
            k.carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                        : std::vector<std::string>{};
        k.vce_wants_pb = std::find(vce_families.begin(), vce_families.end(),
                                   std::string("pb")) != vce_families.end();
        k.vce_wants_cb = std::find(vce_families.begin(), vce_families.end(),
                                   std::string("cb")) != vce_families.end();
        k.streaming_on = nc.streaming.enable;
        k.within_epoch_armed = config_.networkRaimWithinEpoch();
        k.pd_sticky          = config_.networkPhaseDatumSticky();
        k.anc_slip_handover  = config_.networkCycleSlipAnchorSlipHandover();
        k.pd_gate_drop_erase = config_.networkPhaseDatumGateDropErase();
        k.pd_min_stations    = config_.networkPhaseDatumMinStations();
        k.max_duration_s     = config_.getMaxDuration();
        k.epoch_decimation_n = static_cast<std::size_t>(std::max(1, nc.epoch_decimation_n));
        k.rhs_attrib_tow     = nc.rhs_attrib_tow;
        k.rhs_attrib_n_ep    = std::max(1, nc.rhs_attrib_n_ep);
        k.trace_postfit_on   = nc.trace_postfit_sigma0 || nc.trace_postfit_keyed
                               || nc.trace_postfit_rows;
        k.trace_sh_spectrum   = nc.trace_sh_spectrum;
        k.trace_station_block = nc.trace_station_block;
        k.amb_birth_floor_on   = k.amb_float_kf && k.amb_birth_var_floor > 0.0;
    }
    return k;
}

} // namespace GnssProcessor
