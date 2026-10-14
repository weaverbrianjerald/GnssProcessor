/*!
 * \file GnssConfigNetworkPosture.cpp
 * \brief The network posture readout: recommended versus armed knobs, per run regime.
 *
 * Separate from @c GnssConfigLoadNetwork.cpp (which holds @c loadNetworkSection_)
 * because it changes for a different reason than the loader: a card that adds a
 * knob edits the loader, and a card that validates a knob adds a row here, so the
 * two kinds of change touch different files.
 *
 * The readout is log-only: it reads already-parsed state and changes nothing.
 * It is split so that a new recommendation is a one-row edit inside one
 * function:
 *
 *   - @c networkPostureRegime_           : the regime predicates every row filters on
 *   - @c appendNetworkPostureItemsOn_    : the ON tier rows (gaps log a warning)
 *   - @c appendNetworkPostureItemsScoped_ : the SCOPED tier rows (gaps log at info)
 *   - @c networkPostureBundles_          : the coupled bundles (a partial arm warns)
 *   - @c logNetworkPosture_              : the report pass over all of the above
 *
 * @c logNetworkPosture_ appends the ON rows before the SCOPED rows, which keeps
 * the two blocks contiguous in the log.
 *
 * @see docs/CONFIG_promotion_ledger.md, scripts/config_posture_check.py
 */

#include "config/GnssConfig.hpp"
#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace GnssProcessor {

    // =================================================================
    //  Posture readout: what the field record recommends versus what is armed
    // =================================================================
    /**
     * @brief Compute the regime predicates every posture row filters on.
     *
     * A knob validated at one scale can be harmful at another
     * (@c mw_wl.bridge_sample_debias: validated at 50 stations, net-harmful at 305,
     * where the short-baseline wrong-integer rate went from 0.0 % to 12.5 %), so a row
     * is reported only when the regime it was validated in matches this run.  Computed
     * once and passed to every table builder.
     *
     * @return The regime facts for this configuration.
     */
    GnssConfig::PostureRegime GnssConfig::networkPostureRegime_() const {
        const auto&       nc    = network_config_;
        const std::size_t n_sta = nc.stations.size();
        const std::size_t n_sys = active_systems_.size();
        const bool tri   = frequency_mode_ == FrequencyMode::Triple
                        || frequency_mode_ == FrequencyMode::All;
        const bool warm  = !nc.state_snapshot_path.empty();
        const bool lane  = nc.mw_wl.ewl_enable;
        // The real-time / service posture.  service_mode is the RT service producer, so a
        // knob scoped to post-processing (carry_invariant: ~57% of epoch wall time at
        // 305 sta) stays silent there, and the RT-only knobs stay silent everywhere else.
        const bool rt    = app_mode_ == AppMode::RealTime || network_service_mode_;
        // The per-station parallel family is inert without a log line below the station
        // threshold (setup.parallel_on in ApplicationNetworkSetup.cpp), so seam/carry are
        // reported only where the family engages.
        const bool par   = nc.parallel_stations
                        && static_cast<int>(n_sta) > nc.parallel_min_stations;
        // Config-determinable epoch spacing, the same derivation the emit_clock_rate
        // inert-arming guard in GnssConfigLoadNetwork.cpp uses.  0 when gnss_rate_s is 0
        // (native RINEX rate, unknown at load).
        const double spacing_s = gnss_rate_s_ * static_cast<double>(nc.epoch_decimation_n);
        // active_systems_ is a vector of names, and a count does not identify a
        // constellation set: n_sys <= 2 admits GPS+BeiDou (for example
        // config/test_suite/_ppprtk50_4_gps_bds.json), which the W6 code-bias scope
        // excludes.  The predicates below test the names.
        const bool gps_only = n_sys == 1 && active_systems_.front() == "GPS";
        const bool gps_gal_only =
            !active_systems_.empty()
            && std::all_of(active_systems_.begin(), active_systems_.end(),
                           [](const std::string& s) { return s == "GPS" || s == "Galileo"; });

        return PostureRegime{ n_sta, n_sys, tri, warm, lane, rt, par,
                              spacing_s, gps_only, gps_gal_only };
    }

    /**
     * @brief Append the ON-tier recommendation rows (a regime-matched gap logs a warning).
     *
     * ON tier: no known counter-scope in this run's regime.  A missing ON knob otherwise
     * costs a wasted run before it is noticed, so these gaps warn, while the SCOPED gaps
     * (about 90 rows) only inform.
     *
     * @param r    Regime facts from @ref networkPostureRegime_.
     * @param out  Row sink; appended to (call this before the SCOPED builder so the two
     *             blocks stay contiguous in the log).
     */
    void GnssConfig::appendNetworkPostureItemsOn_(const PostureRegime& r,
                                                  std::vector<PostureItem>& out) const {
        // Alias preamble: the rows below use the names the table was written with.
        using Tier = PostureTier;
        using Item = PostureItem;
        const auto&       nc    = network_config_;
        const std::size_t n_sta = r.n_sta;
        const bool        tri   = r.tri;
        const bool        warm  = r.warm;
        const bool        par   = r.par;

        const std::vector<Item> rows = {
            // --- ON tier: the carry couplings -------------------------------------------
            { network_carry_conditional_coupling_ && !network_carry_invariant_,
              network_carry_coupling_gauge_project_,
              Tier::ON, true , "network.carry_coupling_gauge_project",
              "a DEPARTED coupling column is marginalized by DELETING its coefficient, so the "
              "family sum moves to -M_dep and the row observes the CURRENT datum gauge at the "
              "OLD realization. 4-rung ladder (SPEC_card1_coupling_carry sec 13.8): 24-sta sim "
              "+ 50/150/305-sta FIELD, withholds -> 0 and invariant-#7 R down 9-10 decades at "
              "EVERY rung, no runtime cost. 2x2 isolation (sec 13.9): 1f owns availability + "
              "gauge honesty + the sum-to-zero datum check (1.6x tighter); it is wire-NEUTRAL "
              "(the wire/clock quality win is 1g's). Owed: multi-constellation, warm-resume, "
              "real-time" },
            { warm,
              nc.state_snapshot_require,
              Tier::ON, false, "network.state_snapshot_require",
              "without it a fingerprint/age/parse failure SILENTLY cold-starts; mandatory in "
              "every same-snapshot A/B" },
            // --- ON tier: QC, RAIM and slip knobs -------------------------------------
            { network_phase_leveling_ || network_carry_invariant_,
              network_cycle_slip_if_phase_td_,
              Tier::ON, false, "network.cycle_slip.if_phase_td",
              "complementary geometric-jump layer; hardened arm is clean at 0.37% fire rate, "
              "raw-confirmed 71% -> 90%" },
            { network_phase_leveling_ || network_carry_invariant_,
              network_cycle_slip_mw_detect_,
              Tier::ON, false, "network.cycle_slip.mw_detect",
              "the robust coarse detector (WL+LLI, no a-priori position); zero-q alone locked a "
              "wrong integer, rms_clock 2 -> 7-8 m" },
            { network_phase_leveling_ && network_phase_datum_prioritized_,
              nc.cycle_slip_anchor_slip_handover,
              Tier::ON, false, "network.cycle_slip.anchor_slip_handover",
              "hand a phase-bias family over CONTINUOUSLY in the epoch a slip response frees its "
              "anchor arc (the sticky election otherwise re-realizes the family from the freed "
              "arc: the gauge engine of the storm-bed plateau, SPEC_event_harness s12); storm bed "
              "ZWD 1.19 -> 0.05 m, consumer p50 1.09 -> 0.28 m, T4 9,871 -> 306, WL 86 -> 98 %" },
            { network_phase_leveling_ && network_cycle_slip_mw_detect_,
              nc.cycle_slip_mw_evidence_refloat,
              Tier::ON, false, "network.cycle_slip.mw_evidence_refloat",
              "admit a strong, mature, bias-free-lane MW detection to the refloat path whether "
              "or not its integer rounds cleanly (PASS A had re-used the T0 REPAIR gate, frac <= "
              "0.15 on a 5.86 m lane, and dropped correct 99-cycle flags at 78x threshold with no "
              "counter, SPEC_event_harness s13); storm bed with the anchor handover ZWD 0.046 -> "
              "0.007 m, consumer p50 0.28 -> 0.17 m, T4 306 -> 78, WL 98.9 %" },
            { network_phase_leveling_ && network_cycle_slip_mw_detect_,
              nc.cycle_slip_common_band_scope,
              Tier::ON, false, "network.cycle_slip.common_band_scope",
              "free only the band the MW evidence names: a silent, mature pair exonerates its bands unless "
              "IF-TD flagged it, LLI is set, or the pair is in the LLI shadow (rule F2); the anchor clause keeps "
              "a satellite's references co-located; the LLI-epoch verdict (rule F3) scopes every drain to the "
              "receiver's bits (SPEC_event_harness s14, s16): single-band legs 24-300x lower level residual, "
              "nine cold handovers made continuous on the storm bed; STORM BED, FINAL ARM (runs/golden_sim_campaign_zwdblk_cbscope5 = common_band_scope true + if_phase_td_pairs all on main b268ac2 with rules F2, F3 and " },
            { network_phase_leveling_ && network_cycle_slip_mw_detect_,
              nc.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All,
              Tier::ON, false, "network.cycle_slip.if_phase_td_pairs",
              "an IF-TD pair behind EVERY MW lane (per-pair pools, shared reference arm) so the common-band "
              "verdict can veto an equal-count silence on any pair, not only the first (SPEC_event_harness "
              "s14.10, s16): the same-epoch trap S2 vetoed exactly, inert on a single-band slip; STORM BED, FINAL ARM (runs/golden_sim_campaign_zwdblk_cbscope5 = common_band_scope true + if_phase_td_pairs all on main b268ac2 with rules F2, F3 and " },
            // --- The reference-arc clause (s22, PROMOTED): a silent MW pair whose IF-TD
            //     sample is the chain's reference satellite is untested, not clean (dd == 0
            //     by construction, so the pooled preview can never flag it and the veto is
            //     structurally unavailable).  It is a clause of the scope's rule, so its
            //     regime is the scope.
            { nc.cycle_slip_common_band_scope,
              nc.cycle_slip_common_band_ref_arc,
              Tier::ON, false, "network.cycle_slip.common_band_ref_arc",
              "refuse a reference-arc pair as an exonerator (its IF-TD test cannot run): the field "
              "equal-count L1C+L5X slip on GOPE/G24 goes from the wrong narrowing (keep L2W) to "
              "all-band with both reference arcs named; single-band slips elsewhere unchanged, G25 "
              "byte-identical; sim storm arm neutral on every bar (consumer p50 0.0807 both, T4 11 "
              "both, ZWD 0.0059 both; runs/golden_sim_campaign_zwdblk_refarc vs _sts)" },
            { true,
              network_disc_threshold_frac_cycles_ >= 0.24 && network_disc_threshold_frac_cycles_ <= 0.26,
              Tier::ON, false, "network.disc_threshold_frac_cycles",
              "set 0.25: the flat 0.05 m gate is 0.26 cyc on L1 but 0.20 on L5; G3 A/B cut PB "
              "disc L5 -20%, total -10%" },
            // --- ON tier: emission and product knobs ----------------------------------
            { true,
              network_phase_leveling_,
              Tier::ON, true , "network.phase_leveling",
              "armed in 107 of 108 shipped configs; zero-q sharpens clock 2.44 -> 2.00 m (-18%); "
              "the carry_warm gate for RAIM" },
            // RTA-10 H heartbeat record (ledger §A2 F16, applied with h_sigma0_prefit_census):
            // record stream 0-diff, solve-neutral; a tail consumer can tell "withheld" from
            // "producer died".  The row gives the ledger entry its loader coverage.
            { true,
              network_emit_epoch_status_record_,
              Tier::ON, false, "network.emit_epoch_status_record",
              "one positional H heartbeat record per epoch incl. withheld ones (gate + sigma0): "
              "record stream 0-diff, solve-neutral; pair with h_sigma0_prefit_census" },
            { true,
              nc.write_run_manifest,
              Tier::ON, false, "network.write_run_manifest",
              "run provenance: config hash + build stamp + POST-arming toggle dump; no estimation "
              "effect, no product bytes" },
            // --- ON tier: performance / parallelism and network trace_* knobs --------
            { par,
              nc.parallel_seam,
              Tier::ON, false, "network.parallel_seam",
              "byte-identical compare PASS at 50 and 305 sta; 1.64x epoch (76.1 -> 46.5 s/ep) "
              "cold, toward 2x warm" },
            { true,
              nc.trace_epoch_status,
              Tier::ON, false, "network.trace_epoch_status",
              "cost is O(epochs) not O(stations); the only record of WHY an epoch was skipped "
              "(disposition + sigma0)" },
            { true,
              nc.trace_flag_rates,
              Tier::ON, false, "network.trace_flag_rates",
              "O(1) counters + 2 end-of-run files; the only check that the armed QC tiers fire at "
              "their designed alpha" },
            { true,
              nc.trace_section_timers,
              Tier::ON, false, "network.trace_section_timers",
              "measured byte-identical A/B; the only per-epoch cost attribution - every perf "
              "claim needs it" },
            { true,
              nc.trace_prefit_census,
              Tier::ON, false, "network.trace_prefit_census",
              "measured 7.6 ms/epoch (0.4%) at 63 sta, solve byte-identical; the ONLY always-on "
              "PREfit screen - postfit sigma0 launders a parked cell through the free "
              "ambiguities (era-2: 1.9e5 sigma invisible for minutes)" },
            { true,
              nc.trace_topology,
              Tier::ON, false, "network.trace_topology",
              "same walk as the census, event-only (3.5 events/epoch measured); standing "
              "invariant #7 - it is what makes a gate's decouple/re-couple edges visible at all "
              "(13 silent re-coupling edges recovered on FT63 leg-19)" },
            // --- ON tier: cold-start, datum and clock knobs ---------------------------
            { network_phase_datum_eq34_ || network_phase_datum_prioritized_,
              network_station_pd_pin_aware_,
              Tier::ON, false, "network.station_pd_pin_aware",
              "305-sta 3h: 884 spurious pin-driven station rejections -> 0; clock common-mode "
              "0.524 -> 0.376" },
        };
        out.insert(out.end(), rows.begin(), rows.end());
    }

    /**
     * @brief Append the SCOPED-tier recommendation rows (a regime-matched gap logs at info).
     *
     * SCOPED tier: validated, but only inside a stated scope (a station count, a
     * constellation set, a lineage shape).  There are about 90 of them; warning on all
     * would bury the ON tier, so their gaps log at info level and stay searchable.
     *
     * @param r    Regime facts from @ref networkPostureRegime_.
     * @param out  Row sink; appended to (call this after the ON builder).
     */
    void GnssConfig::appendNetworkPostureItemsScoped_(const PostureRegime& r,
                                                      std::vector<PostureItem>& out) const {
        // Alias preamble: the rows below use the names the table was written with.
        using Tier = PostureTier;
        using Item = PostureItem;
        const auto&       nc           = network_config_;
        const std::size_t n_sta        = r.n_sta;
        const bool        warm         = r.warm;
        const bool        lane         = r.lane;
        const bool        rt           = r.rt;
        const bool        par          = r.par;
        const double      spacing_s    = r.spacing_s;
        const bool        gps_only     = r.gps_only;
        const bool        gps_gal_only = r.gps_gal_only;

        const std::vector<Item> rows = {
            // --- SCOPED tier: the carry couplings ---------------------------------------
            { !network_carry_invariant_ && network_phase_leveling_ && network_shared_carry_full_cov_,
              network_carry_conditional_coupling_,
              Tier::SCOPED, true , "network.carry_conditional_coupling",
              "legacy lineage (needs shared_carry_full_cov): 200-ep A/B step p50 0.550 -> 0.103 "
              "cyc, a00 stabilized" },
            { network_carry_conditional_coupling_ && !network_carry_invariant_,
              network_carry_coupling_time_update_,
              Tier::SCOPED, true , "network.carry_coupling_time_update",
              "else the coupling rows re-assert the harvest-epoch border at full strength (~5.8 "
              "cyc, 1.5e4x weight lie)" },
            // --- The exact coupling update and its companions (card CTU, promoted by card
            //     EXL).  Card 1d's scalar update ages each row's satellite clock term at the
            //     legacy sat_clock_q while card Q3's anchor folds the same clock at its own q,
            //     so a young arc's ambiguity information is discarded every epoch (card AIL)
            //     and the clock level freezes (card CTU's root); the exact update re-expresses
            //     each row on the moved border with the fold's own Q.  Its companions are each
            //     measured necessary on this posture (card CTU2 sections 2, 5 and 6); the
            //     guard space (phase) and rank_gate_tol 1e-11 are value companions of the same
            //     ledger row.  Regime: the float vehicle on the leveling carry with the card 1
            //     coupling trio, which the loader requires.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && network_phase_leveling_
                  && network_carry_coupling_time_update_ && network_shared_carry_full_cov_
                  && !network_carry_coupling_joint_whiten_,
              network_carry_coupling_exact_update_ && network_clock_anchor_guard_space_ == 2
                  && network_rank_gate_tol_ <= 1e-11,
              Tier::SCOPED, true , "network.carry_coupling_exact_update",
              "re-express each coupling row on the moved border with the fold's own Q, not "
              "the legacy clock q (a young arc's ambiguity information was discarded every "
              "epoch, 75 mm sim / 157 mm field below 20 epochs, card AIL): sim clock level "
              "0.16 -> 0.02 m, IF code spread sim 0.143 -> 0.057 m, field 0.266 -> 0.164 m; "
              "ships only with the phase-clock hold (field consumer 3.878 / 2.245 m in hours "
              "1 / 2 under the plain hold, 0.108 / 0.024 m under the phase-clock hold); armed "
              "only with guard space phase and rank_gate_tol 1e-11; fresh convergence only "
              "(network.ctu_switch_on_resume off)" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && network_phase_leveling_
                  && network_carry_rx_code_bias_ && network_carry_conditional_coupling_
                  && !network_carry_coupling_joint_whiten_ && !network_rx_cb_carry_demean_
                  && !network_rx_cb_carry_demean_within_ && !network_rx_cb_carry_step_gate_
                  && !network_rx_cb_harvest_demean_,
              network_carry_rx_code_bias_in_block_,
              Tier::SCOPED, true , "network.carry_rx_code_bias_in_block",
              "fold the receiver code-bias carry inside the station's whitened block as card 1 "
              "coupling rows (the diagonal fold at the conditional variance doubled the border "
              "information every epoch, card CTU2 section 2); a companion of the exact update" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              network_amb_float_drop_decouple_always_,
              Tier::SCOPED, true , "network.amb_float_drop_decouple_always",
              "a drop clears the arc's border regression and block row even at birth strength "
              "(without it the field level jumps by metres at the first fold under the exact "
              "update, card CTU2 section 6); a companion of the exact update" },
            { !network_carry_invariant_ && network_phase_leveling_,
              network_shared_carry_full_cov_,
              Tier::SCOPED, false, "network.shared_carry_full_cov",
              "legacy lineage: recovers the clock-to-phase-bias off-diagonals the diagonal stash "
              "drops; 50 sta cost ~0" },
            // --- Write half of datum continuity (RS-3, SPEC_event_harness s21.7): the eq34
            //     chain tree and the time-linked code-bias datum memory are stored in the
            //     snapshot as format-forward rows.  Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty()
                  && (network_phase_datum_eq34_ || network_code_datum_time_linked_),
              nc.state_snapshot_datum_capture,
              Tier::SCOPED, false, "network.state_snapshot_datum_capture",
              "carry the eq34 chain tree + the time-linked code-bias datum memory in the snapshot so a "
              "warm resume can continue both datums (RS-3): first-epoch eq34 re-references 110 -> 8, "
              "code-bias re-seed discontinuities 30 -> 0, R record 30 -> 0 satellites" },
            // --- Write half of emit-layer continuity (RS-5, SPEC_event_harness s21.10/s21.11):
            //     the eq34 emit re-base frame and the PB emission filter state are stored in the
            //     snapshot.  Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty()
                  && (network_carry_invariant_emit_rebase_ || network_emit_pb_filter_),
              nc.state_snapshot_emit_capture,
              Tier::SCOPED, false, "network.state_snapshot_emit_capture",
              "carry the emit layer's memory (ER/PF/PS rows) so a warm resume does not cold-start the "
              "eq34 emit re-base and the PB filter: the seam's 17.7 m per-key phase-bias step becomes "
              "0.000 m p50 (RS-5)" },
            // --- Write half of station-layer continuity (RS-6a, SPEC_event_harness
            //     s21.13/s21.14): the rx-CB carry and the T5 per-LOS deviation carry are stored
            //     in the snapshot (RC/SD rows).  Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty(),
              nc.state_snapshot_station_capture,
              Tier::SCOPED, false, "network.state_snapshot_station_capture",
              "carry the rx-CB carry + the T5 iono deviation carry in the snapshot so a warm resume "
              "continues the station layer's memory: the SH a00 seam transient +4.6 -> +0.4 TECU, "
              "receiver clocks at +1 1.21 -> 0.91 m (RS-6a)" },
            // --- Write half (RS-9, SPEC_event_harness s21.18/s21.19): the rx-CB carry's
            //     linearization point (RL rows beside RC) is stored in the snapshot.  Write
            //     side only, products byte-identical; requires state_snapshot_station_capture.
            { !nc.state_snapshot_path.empty() && nc.state_snapshot_station_capture,
              nc.state_snapshot_rxcb_lin_capture,
              Tier::SCOPED, false, "network.state_snapshot_rxcb_lin_capture",
              "carry the rx-CB carry's code-bias linearization point so a warm resume judges the "
              "restored carry against the writer's frame, not one epoch of resume code (RS-9)" },
            // --- Write half (RS-8, SPEC_event_harness s21.16/s21.20): every station's locked
            //     canonical-signal map (CN rows) is stored in the snapshot.  Write side only,
            //     products byte-identical.
            { !nc.state_snapshot_path.empty(),
              nc.state_snapshot_signal_capture,
              Tier::SCOPED, false, "network.state_snapshot_signal_capture",
              "carry each station's canonical-signal map so a warm resume does not re-decide 23 of "
              "61 stations' channels from the resume epoch: chord births at the seam 64 -> 1 (RS-8)" },
            // Reported only where the lineage carries the ZWD (the F3 station block or
            // carry_zwd).  On a white-sink ZWD (the field golden) the B9 prior is ~4000x too
            // stiff and the harvest turns it into the falsified diagonal carry (RS-10,
            // SPEC_event_harness s21.21).
            { warm && (network_phase_leveling_local_block_ || network_carry_zwd_),
              network_zwd_seam_reseed_,
              Tier::SCOPED, false, "network.zwd_seam_reseed",
              "warm-resume ZWD no-restart (h6 truth rms 0.124 vs cold 0.390); settled h9-12 "
              "consumer 0.134 vs no-Z warm 0.278; NOT fingerprinted; OFF where the lineage carries "
              "no ZWD (RS-10: zwd prior sigma0 11, ZWD leg-seed 0.29 -> 1.34 m by +60)" },
            // --- The seam reseed's own stiffness (RS-10b, SPEC_event_harness s21.21): seam-scoped
            //     and fingerprint-exempt, separate from the fingerprinted zwd_carry_q.
            { warm && network_zwd_seam_reseed_,
              network_zwd_seam_reseed_q_ > 0.0,
              Tier::SCOPED, false, "network.zwd_seam_reseed_q",
              "give the ZWD seam reseed its own stiffness: at the inherited 1e-9 the seam prior is ~4000x "
              "too stiff on a white-sink ZWD (sigma0 11, ZWD leg-seed 1.34 m by +60); tuned, the tails "
              "collapse to the reseed-OFF arm's while the cold consumer keeps its smoother start (RS-10b)" },
            // --- SCOPED tier: QC, RAIM and slip knobs ---------------------------------
            { estimate_network_sat_code_bias_ && !warm,
              network_code_bias_disc_jump_test_,
              Tier::SCOPED, true , "network.code_bias_disc_jump_test",
              "set with a wired CB response: the structural triggers bumped 0 at the measured 49 "
              "m G01 code-bias step" },
            { network_cycle_slip_if_phase_td_,
              nc.slip_qc.ref_gate,
              Tier::SCOPED, false, "network.cycle_slip.if_phase_td_ref_gate",
              "hardened refsat gate: raw-confirmed 71% -> 90%, unsupported refsat flags 14 -> 4, "
              "pool arm untouched" },
            // --- The reference arm on the additional chains (REF-FAN, s22.6/s22.7), so a slip on a
            //     chain's own reference does not fan out; own-elected references go through the
            //     empirical gate, adopted ones inherit the first chain's verdict.  Regime: pairs
            //     all plus the empirical gate.
            { nc.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All && nc.slip_qc.ref_gate,
              nc.slip_qc.ref_arm_all,
              Tier::SCOPED, false, "network.cycle_slip.if_phase_td_ref_arm_all",
              "run the reference arm on the additional IF-TD chains (empirical gate): collateral refloats "
              "6 -> 0 on every GOPE/G24 event, the 13.3 m innocent PB re-realization removed, consumer mean "
              "1.4-2.7x better, flood held (93/94 wide-chain candidates suppressed); sim storm arm owed" },
            // --- The re-entry response on the float vehicle (card G, AB2 sections 12 to 15).
            //     Freeing a re-entering arc makes it re-level from one epoch of code; on the
            //     converging leveling vehicle that is KEEP_OFF (float consumer +56 % in hours
            //     3 to 9), while a float ambiguity state with a floored birth prior prices it
            //     differently.  Regime: the float vehicle.  The KEEP_OFF on the leveling
            //     vehicle stands, so this row does not report there.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              nc.slip_qc.gap_reentry,
              Tier::SCOPED, false, "network.cycle_slip.gap_reentry_relevel",
              "free a re-entering arc's ambiguities on the float vehicle: 12 h field post-fit "
              "refloats 1069 -> 702, epochs below a decimetre 223 -> 321, clock wire on the "
              "consumer's satellites 0.0406 -> 0.0203 m, never worse than 0.03 m of consumer "
              "p50 in hours 3 to 12; needs cycle_slip.relevel_free_epoch to act" },
            // --- The per-signal rider of the row above (card GRS, PROMOTED).  The row-level
            //     tier keys on the satellite, so it does not see a loss of lock on the
            //     primary pair while band 5 or a fallback signal keeps tracking.  Regime: the
            //     float vehicle and the row-level knob; without the latter the per-signal
            //     history is never filled, the loader warns and the tier counts itself inert.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && nc.slip_qc.gap_reentry,
              nc.slip_qc.gap_reentry_per_signal,
              Tier::SCOPED, false, "network.cycle_slip.gap_reentry_per_signal",
              "see the re-entry per SIGNAL, not per satellite: sim fault bed repaired "
              "completely (clock wire against truth 0.0670 -> 0.0030 m dual, 0.0939 -> "
              "0.0029 m triple), field dual T4_attrib 228 -> 92, and the 12 h field arm "
              "never worse than 0.02 m of consumer p50 in hours 3 to 12 (T4_attrib 702 "
              "-> 370)" },
            // --- The tracker key of the two rows above (card GRK).  With the knob off, the
            //     arc-level tracker names each band by the lexically lowest phase carrying a
            //     matching code (L1C before L1W), while a carried ambiguity at an L1W station
            //     is keyed L1W; the row-level re-entry response then looks the carry up under
            //     a key that station never uses, and the band-1 carry folds across every
            //     break there.  Armed, a band whose latched phase is present in the raw
            //     record with its matching code is named by that phase.  Regime: the float
            //     vehicle and the row-level response; without the response there is no
            //     lookup to miss, the choice changes only detections and the trace, and the
            //     loader warns.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && nc.slip_qc.gap_reentry,
              nc.slip_qc.tracker_modeled_first,
              Tier::SCOPED, false, "network.cycle_slip.tracker_modeled_first",
              "the arc-level slip tracker names each band by the station's LATCHED phase, the "
              "key the carry uses (the lexical rule named L1C at every L1W station, so the "
              "row-level re-entry response never reached the band-1 carry there): unreachable "
              "re-entries 21 -> 0, gross post-fit code rows at full weight 507 -> 101 (12 h "
              "759 -> 261), consumer, census and clock wire unchanged; byte-identical on the "
              "sim" },
            // --- Accumulated phase wind-up and its restart response (card WUP).  In
            //     fractional mode each half-cycle crossing steps the applied wind-up by one
            //     cycle on both bands (MW-blind, half of them under the IF-TD floor), about
            //     half the field's hour-scale walk-share excess (card FWS).  Accumulated,
            //     every return after a gap above the wind-up horizon restarts the unwrap,
            //     which the response answers by freeing the arc's leveling carry (invariant
            //     #1).  Regime: the float vehicle on the phase-leveling carry, the posture
            //     both were graded on; the response also needs accumulation (the loader
            //     disarms it otherwise).
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && network_phase_leveling_,
              accumulate_phase_windup_,
              Tier::SCOPED, false, "estimation.accumulate_phase_windup",
              "unwrap the phase wind-up instead of wrapping it: the fractional staircase is 100 "
              "one-cycle steps in 6 h on the field golden (107 mm each in the ionosphere-free "
              "combination, MW-blind), IF-TD flags 111 -> 70; accumulating consumers on the "
              "census 2.6 cm better at the median site in hours 2 to 6 (9 of 17 by more than "
              "2 cm, none worse); needs network.cycle_slip.windup_restart_refloat" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && network_phase_leveling_
                  && accumulate_phase_windup_,
              network_cycle_slip_windup_restart_refloat_,
              Tier::SCOPED, false, "network.cycle_slip.windup_restart_refloat",
              "free the leveling carry of an arc whose wind-up accumulator restarted (a return "
              "after a gap above 300 s): unanswered, the carry folds across an integer-cycle "
              "step; 6 h field golden 27 of 28 in-horizon restarts answered (52 arcs), sim 14 "
              "of 15, none bridged, consumers unchanged" },
            // --- The canonical-signal latch (card CSL).  A value knob, and the first
            //     processing.* row in this table: the default string "count" is truthy, so
            //     the armed test is an explicit comparison on canonical_signal_rule_ (a
            //     direct GnssConfig member, not part of network_config_ and not a
            //     PostureRegime alias), as the apriori_source row compares
            //     network_apriori_source_.  Regime: the float vehicle, the only posture the
            //     rule was graded on.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              canonical_signal_rule_ == CanonicalSignalRule::Priority,
              Tier::SCOPED, false, "processing.canonical_signal_rule",
              "latch each station's band signal by PRIORITY, not by the satellite count "
              "of its first processed epoch (one satellite without the semi-codeless "
              "pair latches the civil signal for the whole run): field dual epochs below "
              "a decimetre 162 -> 603 of 720, 12 h 275 -> 994 of 1440, triple 6 -> 314; "
              "byte-identical on the sim; mechanism half read (a station subset)" },
            // --- The Galileo package (cards GAL2 and GFC, adopted 2026-10-13, ledger A16).
            //     Four processing / estimation / model_corrections value rows; like the CSL
            //     row above, each armed test is an explicit comparison on a direct GnssConfig
            //     member.  Regime: the float vehicle in dual-frequency mode (the package was
            //     graded dual only) on a GPS / Galileo set.  The
            //     active_systems row is in regime on a GPS-only set too (it is the row that
            //     reports a GPS-only float run as a gap); the three rows that only act with
            //     Galileo present need n_sys >= 2 (gps_gal_only keeps the count honest: an
            //     absent active_systems loads all six systems).
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && gps_gal_only,
              std::find(active_systems_.begin(), active_systems_.end(), "Galileo")
                  != active_systems_.end(),
              Tier::SCOPED, false, "processing.active_systems",
              "GPS + Galileo (cards GAL2, GFC): census median -0.020 m against GPS only in "
              "hours 2 to 6 (10 of 16 sites better, 2 worse) and -0.22 m in hour 1; WTZZ00DEU "
              "0.058 0.011 0.018 0.010 0.015 0.011 m by hour against 0.113 0.023 0.016 0.020 "
              "0.030 0.026; 2 to 6 times the GPS-only epoch time depending on the load" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && r.n_sys >= 2
                  && gps_gal_only,
              signal_priorities_.count("Galileo") != 0
                  && signal_priorities_.at("Galileo")
                         == std::vector<std::string>{ "C1C", "C1X", "L1C", "L1X",
                                                      "C5Q", "C5X", "L5Q", "L5X" },
              Tier::SCOPED, false, "processing.signal_priorities.Galileo",
              "E1 / E5a in both code families (pilot C/Q first, combined X second), E5b left "
              "out: the dual-frequency pair the Galileo package was graded on (card GAL2)" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && r.n_sys >= 2
                  && gps_gal_only,
              clock_disc_isb_reinflate_std_ >= 9.99,
              Tier::SCOPED, false, "estimation.clock_disc_isb_reinflate_std",
              "re-inflate the consumer's Galileo inter-system bias to 10 m on a consumed clock "
              "datum discontinuity: without it the first GPS + Galileo sim consumer diverged "
              "to 2.44 m in hour 6 (card GAL); inert without Galileo" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && r.n_sys >= 2
                  && gps_gal_only,
              model_corrections_.enable_receiver_cross_system_atx_fallback,
              Tier::SCOPED, false, "model_corrections.enable_receiver_cross_system_atx_fallback",
              "a receiver antenna without Galileo PCO / PCV takes its nearest GPS band (E1 -> "
              "G01, E5a -> G05) instead of dropping the station's Galileo: 6 field stations "
              "(card GAL2); never applied to a satellite antenna" },
            { true,
              network_exclude_unmodeled_antenna_stations_,
              Tier::SCOPED, false, "network.exclude_unmodeled_antenna_stations",
              "backstop: an unmodelled receiver PCO/PCV is a systematic in every obs feeding the "
              "shared clocks and biases" },
            // --- Roster coordinates held at the monument (card UPO).  A roster station's
            //     configured coordinate is its ITRF2020 monument coordinate while the
            //     observations refer to the antenna, so 48 of the 59 field stations sit below
            //     their antennas by the header eccentricity.  The station's own zenith wet
            //     delay absorbs about a quarter of the height error and the satellite clocks
            //     take the rest, regionally.  Placed beside the antenna-modelling row above
            //     because it is the same class of defect: a station-frame term the shared
            //     states pay for.  Regime: the float vehicle, the only posture the reduction
            //     was graded on; the defect itself does not depend on the vehicle, so the
            //     goldens' re-base owes this knob.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              network_station_marker_to_arp_,
              Tier::SCOPED, false, "network.station_marker_to_arp",
              "reduce the roster's monument coordinates to the antenna reference points at "
              "setup (the network otherwise holds 48 of 59 field stations away from their "
              "antennas, GRAZ00AUT by 1.964 m; the station's wet delay absorbs a quarter and "
              "the satellite clocks the rest, regionally): WTZZ00DEU mean Up -0.085 m -> 0, "
              "hourly p50 from hour 3 0.06-0.09 -> 0.01-0.02 m, clock wire 0.0072 -> 0.0034 m "
              "on the consumer's satellites, 12 h 875 -> 1046 epochs below a decimetre; sim "
              "truth bed restored byte for byte" },
            { network_phase_leveling_,
              network_raim_,
              Tier::SCOPED, false, "network.raim",
              "USER DIRECTIVE Jul-28 armed QC; also feeds the sigma0 EWMA that scales emitted "
              "URA; needs carry_warm" },
            { network_phase_leveling_ && network_raim_ && n_sta >= 50,
              network_raim_attribution_,
              Tier::SCOPED, false, "network.raim_attribution",
              "81% of RAIM events are ONE (station,sat) arc; attribution isolates them instead of "
              "refloating whole sats" },
            { network_phase_leveling_ && network_raim_within_epoch_,
              network_raim_withhold_sigma0_ > 0.0 && network_raim_withhold_sigma0_ >= 1.99 *
                  network_raim_sigma0_trigger_,
              Tier::SCOPED, false, "network.raim_withhold_sigma0",
              "set 6.0 (~2x raim_sigma0_trigger): last-resort backstop; a fired withhold grades "
              "as an adaptation failure" },
            { network_phase_leveling_ && network_raim_,
              network_raim_within_epoch_,
              Tier::SCOPED, false, "network.raim_within_epoch",
              "the ONLY tier that acts BEFORE emit (adapt-and-EMIT); also the only source of the "
              "sigma0_adapt column" },
            // --- The completed gross line-of-sight screen (cards GL / RF-3).  It is the only
            //     acting code-row tier the producer has: every post-fit RAIM pool admits
            //     carried-ambiguity phase rows, so a code-only gross error on one line of
            //     sight is invisible to RAIM's consensus, and on the float vehicle those rows
            //     are the slip response's own re-coupling edge (a drop restores a stale, stiff
            //     variance).  Armed as the three knobs the census graded: the screen, the
            //     one-row rule (the bar reads min(gross_los_min_rows, the LOS's code rows this
            //     epoch), so a line of sight whose station has lost every other code signal is
            //     reachable) and the same-epoch emission exclusion (the decision half runs
            //     ahead of the emission, and the declared LOS leaves its satellite's
            //     contributor set at the declaring epoch).  The hold (gross_los_bound) is
            //     FALSIFIED and the station tier stays off, so the armed test names these
            //     three and nothing else.  Regime: the float vehicle, the only posture the
            //     completed screen was graded on.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              nc.gross_los_screen
                  && nc.gross_los_min_rows_rule == NetworkGrossLosMinRowsRule::Available
                  && nc.gross_los_emit_exclude,
              Tier::SCOPED, false, "network.gross_los_screen",
              "the gross line-of-sight screen completed (the one-row rule and the same-epoch "
              "emission exclusion): the acting code-row tier reaches a line of sight whose "
              "station has lost every other code signal, and the response lands at the "
              "declaring epoch, two epochs earlier than before; gross post-fit code rows above "
              "10 m at full weight 100 -> 32, census 13 of 16 sites better on hours 4 to 6 with "
              "none worse, WTZZ00DEU within 0.003 m, clock wire 0.0045 -> 0.0041 m; the hold "
              "(gross_los_bound) FALSIFIED, the station tier OFF" },
            // (card ICF response 1, armed 2026-10-09 at the author's ruling) The screen's
            // cold-start bound: inside the maturity horizon a line of sight whose post-fit
            // code residual exceeds the bound is declared at once and reconnects on a fresh
            // ambiguity.  Regime: the screen itself.
            { nc.gross_los_screen,
              nc.gross_los_cold_bound_m > 0.0,
              Tier::ON, false, "network.gross_los_cold_bound_m",
              "the cold-start runaway of a faulty receiver channel (BSHM00ISR G07: 11.7 m "
              "without the prior-row refloats, 3 m with them) held within 0.03 m; hours 2 to 6 "
              "clock-wire median +12 % on three arcs, accepted for stability" },
            // --- SCOPED tier: emission and product knobs ------------------------------
            { nc.closed_loop.enable || network_service_mode_,
              network_emit_clock_disc_,
              Tier::SCOPED, true , "network.emit_clock_disc",
              "the ONLY channel carrying the per-sat clock disc counter; without it a "
              "contaminated sat keeps integrating a bad clock" },
            { nc.closed_loop.enable && nc.closed_loop.consume_network_products &&
                  !nc.closed_loop.consume_network_clock && n_sta <= 100,
              network_emit_combined_sigma_ && nc.closed_loop.consume_combined_sigma,
              Tier::SCOPED, false, "network.emit_combined_sigma",
              "with consume_combined_sigma it supersedes the v1 URA deweight cliff (p50 1759x): "
              "vertical 2.99 -> 1.72 m (-42%)" },
            { warm && !nc.product_output_path.empty(),
              network_emit_generation_id_,
              Tier::SCOPED, true , "network.emit_generation_id",
              "a restored non-zero id SUPPRESSES the first-emit mint, so a producer restart does "
              "not hard-reset every consumer" },
            // --- The emit-side phase-bias hold (card PBH).  The emitted phase bias of each
            //     (satellite, signal) is held at the first value of its generation, and a gap
            //     longer than emit_pb_hold_max_gap_s declares a new generation on the wire
            //     (gen= token).  The hold is the last writer of the value and runs one call
            //     ahead of the emitter's own value-jump test, so it changes what the consumer
            //     receives while the solve, the C records and the B records stay
            //     byte-identical to a run with it off.  Regime: the float vehicle and a
            //     closed-loop consumer, the posture it was graded on (the evidence for the
            //     hold is the consumer's).
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && nc.closed_loop.enable,
              nc.emit_pb_hold,
              Tier::SCOPED, false, "network.emit_pb_hold",
              "hold each emitted phase bias at the first value of its generation and declare a "
              "new generation after a gap (the wire's phase-bias movement is harmful error for "
              "a float consumer: 0.0467 m of within-arc RMS on the field against the clock's "
              "0.0274 m, first differences uncorrelated with the clock's): adopted 2026-09-20, "
              "all five PBH-1 bars pass; the solve is untouched" },
            // --- The phase-clock hold (card LAH, promoted by card EXL with the exact update).
            //     The plain hold freezes each emitted phase bias while the exact update moves
            //     the clock and the phase biases together as the level converges, so the
            //     convergence lands on the consumer's phase rows; this hold transfers each
            //     held satellite's level step to its phase bias.  An INTERIM hold: it is valid
            //     only with a precise a priori clock product (the loader warns under
            //     apriori_source brdc_rts).  Emit-only (wire token pbhold=2).  Regime: the
            //     plain hold's, with the hold armed.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && nc.closed_loop.enable && nc.emit_pb_hold
                  && network_apriori_source_ != NetworkAprioriSource::BrdcRts
                  && network_apriori_source_ != NetworkAprioriSource::Sp3BrdcClock
                  && !network_clock_level_carry_ && !nc.fast_clock.enable
                  && !nc.emit_pb_hold_smoothed,
              nc.emit_pb_hold_phase_clock,
              Tier::SCOPED, false, "network.emit_pb_hold_phase_clock",
              "transfer each held satellite's level step to its phase bias (the plain hold "
              "puts the exact update's level convergence on the consumer's phase rows: field "
              "consumer 3.878 / 2.245 m in hours 1 / 2 under the plain hold, 0.108 / 0.024 m "
              "with this hold); armed with network.carry_coupling_exact_update; interim, "
              "precise a priori clock only; the solve is untouched" },
            // --- The consumer's answer to a held bias's presence edges (card PBF).  Under the
            //     hold, a bias that appears on a phase row the consumer already uses, or leaves
            //     it, steps the row by the whole bias; the hold seam claims the key and, without
            //     this knob, the A5 edge is skipped and the Kalman peel is the only response.
            //     Consumer-side only (derived into the consumer as
            //     processing.ssr_pb_hold_presence_flip; never reaches the producer's solve).
            //     Regime: the hold's own (the float vehicle with a closed-loop consumer) and the
            //     hold armed, the only wire whose keys the hold seam claims.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && nc.closed_loop.enable && nc.emit_pb_hold,
              nc.closed_loop.pb_hold_presence_flip,
              Tier::SCOPED, false, "network.closed_loop.pb_hold_presence_flip",
              "answer a held phase bias's presence edges at the consumer (appearing on, or "
              "leaving, a row in use steps it by the whole bias; unanswered, the Kalman peel "
              "misattributes it and ARHT00ATA left by 490 m in one epoch): knob off "
              "byte-identical, the 8 census edges answered and 15 of 17 sites unchanged, "
              "ARHT00ATA 804 -> 0.76 m and QUIN00USA 0.79 -> 0.58 m in hour 2 on the CLV wire" },
            { network_raim_ && (nc.closed_loop.enable || network_service_mode_),
              network_emit_ura_,
              Tier::SCOPED, true , "network.emit_ura",
              "per-sat quality channel a consumer weights on; also the only source of a "
              "non-sentinel sigma0_ewma in health.csv" },
            { true,
              nc.epoch_decimation_n <= 2,
              Tier::SCOPED, true , "network.epoch_decimation_n",
              "n=1 for decisive grading, n=2 is the blessed 60 s dev tier; at n=10 the carry "
              "prediction is INVALID (33 pct/ep refloat)" },
            { network_phase_leveling_ && gnss_rate_s_ > 0.0,
              network_phase_leveling_max_gap_s_ >= 2.0 * gnss_rate_s_ *
                  static_cast<double>(nc.epoch_decimation_n),
              Tier::SCOPED, true , "network.phase_leveling_max_gap_s",
              "a gap guard under 2x the epoch spacing drops the carry on any missed epoch; 120 at "
              "30 s, 900 in the 300 s tier" },
            // --- SCOPED tier: performance / parallelism and network trace_* knobs ----
            { true,
              nc.health_interval_s > 0.0,
              Tier::SCOPED, false, "network.health_interval_s",
              "set 60-300 s: the ONLY mid-run-flushed CSV, so a crashed or WER-zombied run loses "
              "every other tail" },
            { par && !nc.parallel_reduce,
              nc.parallel_carry,
              Tier::SCOPED, false, "network.parallel_carry",
              "2.13x wall-clock (48.6 -> 22.8 s/ep at 20 thr); reassociates ~1e-4, so graded "
              "verdicts still need OFF" },
            { par && nc.parallel_carry,
              nc.parallel_max_threads > 0,
              Tier::SCOPED, false, "network.parallel_max_threads",
              "pcarry partials combine in thread-COUNT order; 0 = all cores makes a counter/hash "
              "ref machine-bound" },
            { nc.parallel_stations && n_sta >= 4,
              static_cast<int>(n_sta) > nc.parallel_min_stations,
              Tier::SCOPED, false, "network.parallel_min_stations",
              "a STRICT > gate with no warning: roster <= this silently kills seam, carry and "
              "parallel ingest" },
            { !rt && n_sta <= 100,
              nc.trace_dd_fractional,
              Tier::SCOPED, false, "network.trace_dd_fractional",
              "grade_ddfrac is the primary honest DD/AR instrument; every DD verdict on record is "
              "stated against it" },
            { nc.trace_dd_fractional && n_sta >= 100,
              nc.trace_dd_fractional_max_pairs > 0,
              Tier::SCOPED, false, "network.trace_dd_fractional_max_pairs",
              "unbounded dd-frac cost 20-40 min/epoch at 305 sta (10-ep run died at ep 4); set "
              "500, or 2000 decimated" },
            { nc.trace_dd_fractional,
              nc.trace_dd_fractional_ref != NetworkDdFracRef::Lowest,
              Tier::SCOPED, false, "network.trace_dd_fractional_ref",
              "every chord carries the ONE elected satellite's own between-station error; 'lowest' "
              "pinned G01/E02 all day on the sim and their eclipse returns read as a network-wide DD "
              "episode.  'oldest' improves the AS-TRACED grade 25-42 % at a returned-reference hour "
              "(E h5 0.334 -> 0.195 cyc) but lands 2-3x ABOVE the ref-free grade - quieter, never "
              "reference-FREE.  Arm it only when the as-traced CSV must stand alone; for a GRADE use "
              "simnet_dd_reref.py / --ref-free instead, and keep goldens on 'lowest' for continuity" },
            { nc.trace_dd_fractional && network_phase_datum_prioritized_,
              nc.trace_phase_anchor,
              Tier::SCOPED, false, "network.trace_phase_anchor",
              "the lane spectrometer's ANCHOR partition is untestable without the "
              "(cons,signal,sat)->anchor-station map; no other artifact carries it" },
            { true,
              nc.trace_events,
              Tier::SCOPED, false, "network.trace_events",
              "pivot handover and retro-bump arming are otherwise SILENT; budget rows at 305 sta "
              "(~1e3 QC events/ep)" },
            { true,
              nc.rhs_attrib_on_trip,
              Tier::SCOPED, false, "network.rhs_attrib_on_trip",
              "the next discharge names its own injector in the run it happens (validated: "
              "reproduced the era-2 attribution 0.002 -> +6493 with NO armed TOW); SCOPED on "
              "MEMORY - it retains one epoch of assemblies + kernel blocks (25.4 MB at 63 sta) "
              "and self-disables above rhs_attrib_prev_max_stations" },
            { network_raim_ && network_phase_leveling_ && n_sta <= 50,
              nc.trace_raim_redundancy,
              Tier::SCOPED, false, "network.trace_raim_redundancy",
              "the ONLY artifact that separates per-SIGNAL Baarda r_i - the RAIM cell is a max "
              "OVER signals, so trace_station_residuals structurally cannot. Solve-NEUTRAL "
              "(computes r_i without using it), unlike raim_redundancy_normalize.  "
              "NEEDS A NON-EMPTY POOL: header-only under carry_invariant unless "
              "raim_carry_invariant re-hosts it.  MEASURED at 305 sta: 14397 rows/epoch, "
              "~5.2M rows / 508 MB per 3 h, epoch wall 93.7 -> 199.6 s with the re-host + "
              "census armed.  Investigation-only, not a standing trace" },
            { nc.trace_epoch_status && !network_raim_within_epoch_,
              nc.trace_postfit_sigma0,
              Tier::SCOPED, false, "network.trace_postfit_sigma0",
              "with raim_within_epoch OFF the sigma0 column is 0.0000 on every row - no post-fit "
              "statistic at emit" },
            // --- SCOPED tier: cold-start, datum and clock knobs -----------------------
            { rt && !(estimate_network_sat_code_bias_ && !estimate_network_iono_sh_),
              network_apriori_source_ == NetworkAprioriSource::BrdcRts,
              Tier::SCOPED, true , "network.apriori_source",
              "self-sufficient RT service needs apriori_source=brdc_rts; float 2.84 m vs sp3 2.76 "
              "m (3 sta/121 ep)" },
            { network_apriori_source_ == NetworkAprioriSource::BrdcRts &&
                  !network_clock_datum_sum_to_zero_,
              network_clock_datum_broadcast_anchored_,
              Tier::SCOPED, false, "network.clock_datum_broadcast_anchored",
              "brdc_rts absolute clock frame: set-event step -0.0003 m vs s2z +0.334 m; "
              "consumer-NEUTRAL, not a position win" },
            { estimate_network_sat_code_bias_ && !network_clock_signal_basis_hold_,
              network_code_datum_time_linked_,
              Tier::SCOPED, true , "network.code_datum_time_linked",
              "W6 code datum pinned in TIME not level: cures the ~0.92 m hysteretic OSB/clock "
              "wander after each rise/set" },
            // --- Card GFC's refinements of the time-linked datum above (the Galileo package,
            //     adopted 2026-10-13, ledger A16).  network.code_datum_pair_continuity is
            //     KEEP_OFF (it preserves the birth realizations; pilot-family users worse) and
            //     has no row: this table lists RECOMMEND_* ledger verdicts only.  Both rows
            //     act on Galileo band 1 alone and are inert on a fixture without the combined
            //     (X) family (the sim), which arming does not change.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && r.n_sys >= 2
                  && gps_gal_only && estimate_network_sat_code_bias_
                  && network_code_datum_time_linked_,
              network_code_datum_family_difference_,
              Tier::SCOPED, true , "network.code_datum_family_difference",
              "one Galileo clock datum across the two code families (the X channel's "
              "sum-to-zero row replaced by the family difference): field Galileo clock wire "
              "0.105 -> 0.035 m, the two inter-family groups collapse, census neutral" },
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf && !r.tri && r.n_sys >= 2
                  && gps_gal_only && estimate_network_sat_code_bias_
                  && network_code_datum_time_linked_ && network_code_datum_family_difference_,
              network_code_family_birth_prior_sigma_m_ >= 0.099
                  && network_code_family_birth_prior_sigma_m_ <= 0.101,
              Tier::SCOPED, true , "network.code_family_birth_prior_sigma_m",
              "a first-sighting family prior at 0.10 m (the family difference's veteran mean): "
              "truth-bed code error from hour 3 0.83 -> 0.05 m, field Galileo clock wire 0.022 "
              "m (level with GPS); hour-2 bars missed (convergence of births at ignition)" },
            { lane,
              network_cold_clock_seed_,
              Tier::SCOPED, false, "network.cold_clock_seed",
              "lane/WL layers difference ~3e5-magnitude x_lin; measured 269 km at 305 sta cold" },
            { n_sta >= 3 && gps_gal_only,
              estimate_network_sat_code_bias_,
              Tier::SCOPED, true , "network.estimate_sat_code_bias",
              "W6 OSB is a drop-in for the .BIA product (3-sta closed loop 0.508 m vs 0.489 m "
              "3D); grade BIAS-ONLY" },
            // --- The second code channel (card SCC).  Placed beside the satellite code-bias
            //     row above because it is the same product: with one canonical band-1 code
            //     per station, the C1C and C1W satellite families compete for the same 59
            //     stations, so a satellite's column can depend on a single tracker.  Armed, a
            //     station contributes every allow-listed band-1 code it tracks as code-only
            //     rows, each through its own receiver inter-signal state.  Regime: the float
            //     vehicle (the only posture it was graded on) and the two dependencies the
            //     loader enforces: without carry_rx_code_bias the new receiver state
            //     cold-starts every epoch and the loader disarms the knob, and with
            //     channel_mode 'all' the loader refuses it because that mode already widens
            //     every band on the phase side.  The knob is armed although it fails a frozen
            //     bar of its own card (B1, WTZZ00DEU): the verdict records the author's
            //     decision and the card keeps the bar as failed.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && network_carry_rx_code_bias_
                  && channel_mode_ != ChannelMode::All,
              nc.second_code_channel,
              Tier::SCOPED, false, "network.second_code_channel",
              "a station contributes every band-1 code it tracks as code-only rows through a "
              "receiver inter-signal state (the priority latch otherwise leaves the wire's "
              "C1W code biases on 18 stations): census hour 1 better by a median of 0.136 m, "
              "code-bias dropouts 50 -> 4, QUIN00USA's excursion gone, median count 282 -> "
              "331.5; costs WTZZ00DEU about 0.02 m from hour 3 and FAILS card SCC's frozen "
              "B1, armed as the author's decision; scale 10" },
            { network_phase_datum_prioritized_ && !network_phase_datum_eq34_ &&
                  network_station_pd_pin_aware_ && n_sta <= 100,
              network_phase_datum_min_stations_ >= 2,
              Tier::SCOPED, false, "network.phase_datum_min_stations",
              "set 2 on the prioritized leg (with pin_aware): trims 394 -> 362 thin PB states; "
              "REFUTED under eq34" },
            { !network_phase_datum_eq34_ && !network_carry_invariant_ && network_phase_leveling_,
              network_phase_datum_prioritized_,
              Tier::SCOPED, true , "network.phase_datum_prioritized",
              "legacy leg when eq34 is off: cures the single-pivot gauge re-reference (G29 -44 m "
              "step, JPLM masked)" },
            // --- The ambiguity vehicle (cards RF-1 / RF-2b / RF-0hv; ledger row added with
            //     the golden re-base, card GRB).  float_kf carries each (station, satellite,
            //     signal) ambiguity as a float state with covariance in the station block
            //     instead of a zero-q scalar level, and the held S-basis freezes the phase
            //     datum's basis value with every handover declared.  The pair is the
            //     posture, not a knob inside one, so the regime is unconditional and the
            //     armed test is the value pair: the retired leveling goldens read as unarmed
            //     here and the refound ones as armed.  Placed beside the prioritized-datum
            //     row because the held basis is that datum's basis (the loader requires the
            //     two together).
            { true,
              nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && network_phase_datum_held_basis_,
              Tier::ON, true , "network.ambiguity_vehicle",
              "the float vehicle with the held S-basis: the working posture since 2026-09-15 "
              "and the goldens' posture since the re-base of 2026-09-22; field 6 h consumer "
              "3.77 1.08 1.28 1.25 1.78 2.68 -> 0.560 0.059 0.028 0.030 0.036 0.045 m by hour "
              "against the leveling posture" },
            { network_phase_datum_prioritized_ && !network_phase_datum_eq34_,
              network_phase_anchor_carry_reref_,
              Tier::SCOPED, false, "network.phase_anchor_carry_reref",
              "QUEUE 1(d): the PHASE_ANCHOR handover re-realizes the datum on the design matrix "
              "ONLY, so the carry folds in the RETIRED frame and the clock absorbs it (1e10:1). "
              "24-sta/1h/GPS sim: clock episodes 7 -> 1, sum|dev| 160.5 -> 1.5 m, datum-free C "
              "contrast 18.66 -> 0.50 m. n=1, field arm OWED; eq34 owns eq34_reref instead" },
            // --- The drop edge of the phase_datum_min_stations coverage gate (card EV7).
            //     The gate assumes a gated-out key costs nothing because the surviving
            //     station's float ambiguity absorbs the removed bias.  On the float vehicle
            //     that ambiguity is a carried state at a centimetre prior: it absorbs the
            //     whole bias in one epoch, and the satellite clocks take up what the prior
            //     does not.  Regime: the float vehicle, the gate, and the re-reference erase
            //     this knob relies on; without phase_anchor_carry_reref the edges are
            //     counted and nothing is erased (the loader warns), and at min_stations 1
            //     no edge can exist.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf
                  && network_phase_datum_min_stations_ >= 2
                  && network_phase_anchor_carry_reref_,
              network_phase_datum_gate_drop_erase_,
              Tier::SCOPED, false, "network.phase_datum_gate_drop_erase",
              "erase a gated phase-bias family's carried ambiguities at the coverage gate's "
              "DROP edge (the survivor's carried ambiguity otherwise absorbs the removed bias "
              "in one epoch, metres against a centimetre prior): field 6 h clock-anchor trips "
              "50 -> 1 and no all-satellite decouple (3), clock wire 0.0107 -> 0.0072 m on the "
              "consumer's satellites, 13 of 16 census sites better, 12 h trips 134 -> 8; "
              "byte-identical on the sim" },
            { !network_phase_datum_prioritized_ && !network_phase_datum_eq34_
                  && network_phase_leveling_ && network_cycle_slip_mw_detect_,
              network_cycle_slip_datum_station_detect_,
              Tier::SCOPED, true , "network.cycle_slip.datum_station_detect",
              "closes the pivot slip blind spot (SPEC_synthetic_minimum_network 9.7.10) at ZERO "
              "withhold cost (vs 97/240 for phase_datum_prioritized): sim fault arm C rms 1.66 -> "
              "0.48 m, spurious CLK_STEP marks 12 -> 1" },
            { gps_gal_only,
              network_sbasis_complete_,
              Tier::SCOPED, false, "network.sbasis_complete",
              "GPS+Gal adds per-sat channel-family PB<->iono trades the declared dimension does "
              "not count; the armed net when the KP lift falls short" },
            // --- Pivot departure (SPEC_event_harness §19, PROMOTED).  By default a pivot
            //     handover's carry skip, oscillator clear and jump-test suppression apply
            //     network-wide, so one constellation's datum change re-realizes the other's
            //     carry: a producer-internal re-realization, not a datum move the consumer
            //     could transform away.  Needs a pivot datum (inert under the anchor-free
            //     ones) and a second constellation.  Fingerprinted: fresh convergence only;
            //     no warm-resume A/B exists.
            { r.n_sys >= 2 && !network_clock_datum_sum_to_zero_
                  && !network_clock_datum_broadcast_anchored_,
              network_scoped_handover_response_,
              Tier::ON, true , "network.scoped_handover_response",
              "scope the pivot-handover carry skip to the constellation whose datum changed: "
              "collateral re-realization is the user's metre-class shock (storm arm settled "
              "p50 0.1204 -> 0.0825 m, max |3D| 1.34 -> 0.20, hour 5 0.236 -> 0.047; fresh "
              "pair edge 1.143 -> 0.117 m) while the producer's own contrasts move < 0.5 % "
              "(runs/golden_sim_campaign_zwdblk_scoped, exe F4FF8EF0)" },
            // --- Pivot departure, increment 1 (SPEC_event_harness §19.6, PROMOTED).
            //     Increment 0 above keeps a handover from invalidating the constellations
            //     whose datum did not move; this row re-expresses the changed
            //     constellation's own carry along the structural clock null direction and
            //     folds it, instead of dropping it for one epoch.  Needs a pivot datum (no
            //     handover seam without one) and its own prerequisite; no n_sys term,
            //     because it acts on the constellation that changed, so one is enough.
            //     Fingerprinted (armed-only token): fresh convergence only.
            { network_scoped_handover_response_
                  && !network_clock_datum_sum_to_zero_
                  && !network_clock_datum_broadcast_anchored_,
              network_handover_stransform_,
              Tier::ON, true , "network.handover_stransform",
              "re-express the changed constellation's carry at a pivot handover instead of "
              "dropping it for an epoch: its own code-bias and phase-bias steps fall 250-575x "
              "back to control (storm arm G PB L1C 0.19914 -> 0.00079 m, G CB C5Q 0.16678 -> "
              "0.00029) and every follow-on consumer reset goes to zero, with ZWD 0.0066 -> "
              "0.0059, L1C 90.2 -> 90.4 and all truth contrasts within 1.6 % "
              "(runs/golden_sim_campaign_zwdblk_sts, exe B3A7E11B)" },
            // Sticky clock-pivot election (RTA-1, STABILITY E3; SPEC_synthetic_minimum_network
            // §18.5/§18.8).  Recommended where per-satellite re-realizations at re-admission
            // are material: triple-frequency and weak-third-signal postures (settled 24 h
            // consumer 3D rms 0.303 -> 0.078 m on the sim bed).  On the GPS dual-frequency +
            // SH field posture the mechanism transfers and does no harm, but the evidence does
            // not support recommending it there.  Inert under the anchor-free clock datums.
            // Fingerprinted: fresh lineage only.
            { r.tri && !network_clock_datum_sum_to_zero_ && !network_clock_datum_broadcast_anchored_,
              network_sticky_pivot_,
              Tier::SCOPED, true , "network.sticky_pivot",
              "sticky clock-pivot election: 8 -> 2 pivot handovers/day, the h16-h18 pivot-eclipse "
              "consumer excursion eliminated (settled 24 h 3D rms 0.303 -> 0.078 m); "
              "triple-freq / weak-third-signal postures" },
            // --- SCOPED tier: the ZWD carry on an F3 lineage ----------------------------
            // Reported only where the ZWD carry is armed on an F3 lineage: without either half
            // there is no diagonal row to replace, and the loader interlock has already logged
            // the case (SPEC_amb_iono_split_overcount §6.16.7d, arms Q/Z/F/F2).
            { network_carry_zwd_ && network_phase_leveling_local_block_,
              network_carry_zwd_in_block_,
              Tier::SCOPED, true , "network.carry_zwd_in_block",
              "MANDATORY with carry_zwd under F3: the diagonal ZWD row is the same station-common "
              "double count F3 exists to remove — quiet sim consumer 3D rms 0.723 m diagonal vs "
              "0.0877 m in-block, field T3c 7619 vs 849 flags; field F3 consumer 3D -27 %, "
              "vertical DC bias +2.83 -> +0.07 m" },
            { network_phase_leveling_local_block_ || network_phase_leveling_los_scale_,
              network_phase_datum_handover_max_var_m2_ >= 0.0099,
              Tier::SCOPED, false, "network.phase_datum_handover_max_var_m2",
              "under F1/F3's honest sigma the lineage's 1e-4 m^2 PB-gauge handover ceiling refuses "
              "93/93 successors (cold handovers, 4-14 m PB frame steps); 1e-2 restores 70/93 and "
              "the settled window — set >= 1e-2 (a working value, not a promotion)" },
            // Resume frame and coupling-regressor restore (SPEC_synthetic_minimum_network
            // §18.10–§18.14).  On a warm resume, restore the phase-anchor incumbents and sticky
            // targets, the Q3 per-satellite anchor state, the CLK-R held frame, the Card-1
            // regression of every restored leveling row, and the dense D2 border covariance
            // (QK/QP), so the first warm fold is conditional and dense.  Leg E: consumer h12
            // 0.62 m rms (p50 0.10) against 2.05 with the frame and regressors alone, and 0.52
            // for a cold consumer on the continuous wire; first-fold clock trips 33 -> 1.
            // Warm-resume lineages only (a writer armed with state_snapshot_frame_capture and
            // state_snapshot_coupling_capture).  Forward-only resume policies, not fingerprinted.
            { warm,
              network_resume_frame_restore_,
              Tier::SCOPED, false, "network.resume_frame_restore",
              "warm resume restores the realization FRAME (anchor incumbents + sticky targets, Q3 "
              "per-sat anchor state, CLK-R held frame): PB sigma 1.11 -> 0.008 m at the first warm "
              "epoch, 2862 -> 54 carries erased; pair with resume_coupling_restore" },
            { warm && network_resume_frame_restore_,
              network_resume_coupling_restore_,
              Tier::SCOPED, false, "network.resume_coupling_restore",
              "warm resume rebuilds the Card-1 regressors + the dense border from a v3 (QK/QP) "
              "snapshot: first-fold clock trips 33 -> 1, consumer h12 2.05 -> 0.62 m rms (a cold "
              "consumer on the wire converges like on the continuous run)" },
            // --- Datum restore at the seam, the read half (RS-3, SPEC_event_harness s21.7):
            //     continue the writer's eq34 tree and code-bias datum instead of re-electing and
            //     re-seeding them.  Forward-only resume policy, not fingerprinted; requires
            //     resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_datum_restore_,
              Tier::SCOPED, false, "network.resume_datum_restore",
              "restore the eq34 tree maps and the code-bias datum memory at a warm resume (RS-3): "
              "eq34 rerefs at +1 110 -> 8, CB re-seed discs 30 -> 0, R record 30 -> 0 sats; a ~10 m "
              "phase-bias re-realization remains and is the next card (SPEC_event_harness s21.9)" },
            // --- Phase-bias seam declaration, the declaration half of the same seam (RS-4,
            //     SPEC_event_harness s21.9): the first post-resume emit marks datum_changed for
            //     any constellation whose phase-bias level re-realized against the restored
            //     emitter baselines.  The CLK-R resume_seam rule cannot see this step: it fires
            //     only on a cold held-frame engine, which a lineage that restores the frame
            //     never has.  Outputs only; forward-only resume response policy, not
            //     fingerprinted.
            { warm,
              network_resume_pb_seam_mark_,
              Tier::SCOPED, false, "network.resume_pb_seam_mark",
              "declare the warm-resume PHASE-BIAS seam on the wire: the first post-resume emit "
              "marks datum_changed for a constellation whose restored per-key baselines moved "
              "past discThresholdM on more than 10 % of the jump test's own population "
              "(measured ~17 m p50 with datum_changed 0 today, SPEC_event_harness s21.9)" },
            // --- Emit-engine restore at the seam, the read half (RS-5, SPEC_event_harness
            //     s21.10/s21.11): both emit engines are restored.  Forward-only resume policy,
            //     not fingerprinted; requires resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_emit_restore_,
              Tier::SCOPED, false, "network.resume_emit_restore",
              "restore the eq34 emit re-base frame + the PB emission filter at a warm resume: emitted "
              "phase biases match the continuous wire to p50 0.000 m at +1, a stateful consumer sees 0 "
              "PB resets at the seam (control 19); products otherwise byte-identical (RS-5)" },
            // --- Station-layer restore at the seam, the read half (RS-6a, SPEC_event_harness
            //     s21.13/s21.14): both station-layer memories are restored.  Forward-only resume
            //     policy, not fingerprinted; requires resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_station_restore_,
              Tier::SCOPED, false, "network.resume_station_restore",
              "restore the rx-CB carry + the T5 deviation carry at a warm resume: a00 transient "
              "+4.6 -> +0.4 TECU, +2 clock contrast 0.44 -> 0.29 m, receiver clocks 1.21 -> 0.91 m; "
              "the code-bias level and the rx-CB gate share do not move (RS-6a)" },
            // --- Canonical-signal map restore, the read half (RS-8, SPEC_event_harness
            //     s21.16/s21.20): the map is restored before any station's first epoch.
            //     Forward-only resume policy, not fingerprinted; requires resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_signal_canon_restore_,
              Tier::SCOPED, false, "network.resume_signal_canon_restore",
              "restore the canonical-signal latch at a warm resume: chord births at +1 64 -> 1, L2X "
              "keys 25.6 -> 0.62 m and every other key 1.50 -> 0.60 m raw, eq34 rerefs 8 -> 2, "
              "clock contrast at +1 0.31 -> 0.16 m (RS-8)" },
            // --- Discontinuity counters on the wire (RTA-4; ledger row added with RS-4): the
            //     per-key discontinuity counters are written as optional P/B/C fields.  Outputs
            //     only; it changes product bytes, so the goldens pin it only with a recorded
            //     re-baseline.
            { true,
              network_serialize_disc_counters_,
              Tier::SCOPED, false, "network.serialize_disc_counters",
              "serialize the per-key phase / code / clock discontinuity counters so a FILE consumer "
              "sees the re-realizations the in-process consumer reacts to (smoke: only P/B/C records "
              "change, b9da8cc1 -> 87a33bf0); pin on the goldens at the RS-5 close-out re-baseline" },
            // Seed-first re-admission (E2; SPEC_synthetic_minimum_network §18.13/§18.14,
            // SPEC_amb_iono_split_overcount §6.10/§6.11).  A departing satellite's D2 clock and
            // code-bias carry is parked and seeds its re-registered columns, so the Q3 anchor
            // folds over the true gap instead of tripping `gap` and freezing a code-level
            // re-draw into a metre-class tail.  Validated on the 24 h sim bed: T3 clock tails
            // 2-4x smaller, consumer post-eclipse excursion 2-3x smaller, DD unchanged.  The
            // clock-only variant (readmit_seed_code_bias false) is FALSIFIED (the tails
            // return), so both kinds seed.  Regime: Q3-anchored lineages without clock_model.
            // Not fingerprinted.  readmit_coupling_marg_ep and
            // phase_datum_handover_max_var_ratio stay KEEP_OFF and have no posture row.
            { network_clock_temporal_anchor_ && !network_clock_model_,
              network_readmit_seed_horizon_s_ > 0.0,
              Tier::SCOPED, false, "network.readmit_seed_horizon_s",
              "E2 seed-first re-admission: parks the departed satellite's D2 clock + code-bias carry "
              "and seeds the return (T3 clock tails 2-4x smaller, consumer post-eclipse excursion "
              "2-3x smaller on the 24 h sim bed); Q3-anchored lineages, 3900 s covers the eclipse "
              "exclusions" },
            { network_readmit_seed_horizon_s_ > 0.0,
              network_readmit_seed_code_bias_,
              Tier::SCOPED, false, "network.readmit_seed_code_bias",
              "readmit seeds cover the code-bias columns too (default true): the clock-only variant "
              "is FALSIFIED — the IF-pair code-bias seeds anchor the clock-vs-code-bias split at the "
              "return, without them the T3 clock tails come back (runs/readmit24h_e2clk)" },
            // Coupling reinit sources (cards 1f/1g).  Meaningful only where the Card-1
            // coupling rows exist: the conditional carry armed and not superseded by the
            // Card-4 invariant carry (under carry_invariant the whole coupling trio is
            // structurally inert).  This row and the carry_coupling_gauge_project row in the
            // ON table are both fingerprinted.  Feed interlock: without eq34 or the
            // prioritized phase-anchor carry re-reference there is no realization-step source,
            // and the card is inert (measured).
            { network_carry_conditional_coupling_ && !network_carry_invariant_
                  && (network_phase_datum_eq34_ || network_phase_anchor_carry_reref_),
              network_carry_coupling_reinit_sources_,
              Tier::SCOPED, true , "network.carry_coupling_reinit_sources",
              "the coupling reinit history was fed ONLY by clock-QC sources, so a phase-anchor / "
              "eq34 / datum-slip re-reference left every OTHER carried local folding that column "
              "at FULL weight with a retired-frame s_prev. 2x2 isolation (SPEC sec 13.9): 1g "
              "owns the PRODUCT-QUALITY win on the CLOCK channel — sim datum-free clock "
              "contrast rms 4.157 -> 0.435 m alone (~10x), field matched-epoch wire 5.631 -> "
              "1.532 cyc/ep; real accuracy (holds on the datum-free contrast), not "
              "over-retention. ⚠ NEVER alone (measured): zero epochs recovered and the eq34 "
              "gauge breach 6.6x HOTTER (R 1.6e+03 -> 1.0e+04) — arm WITH "
              "carry_coupling_gauge_project. Clock-truth instrument is sim-only" },
        };
        out.insert(out.end(), rows.begin(), rows.end());
    }

    /**
     * @brief The coupled bundles: sets of knobs that are correct only when armed together.
     *
     * Arming a strict subset is its own failure mode (the MW/WL lane stack without
     * @c cold_clock_seed quarantines its lane cycles; @c x_lin measured 269 km at 305
     * stations), so an engaged bundle that is partially armed logs a warning, while one
     * with no member armed, or with every member armed, logs at info level.
     *
     * @param r  Regime facts from @ref networkPostureRegime_.
     * @return The bundle table for this configuration.
     */
    std::vector<GnssConfig::PostureBundle>
    GnssConfig::networkPostureBundles_(const PostureRegime& r) const {
        // Alias preamble: the rows below use the names the table was written with.
        const auto& nc   = network_config_;
        const bool  lane = r.lane;

        return {
            { lane, "MW/WL lane stack",
              "the lane rows are built in the CORRECTION frame, so an unseeded cold receiver "
              "clock lands in the ambiguity seeds and lane_max_abs_xlin_m quarantines the "
              "cycles (x_lin 269 km measured at 305 sta); the lane also rides the Card-4 "
              "chord basis",
              { { "network.cold_clock_seed", network_cold_clock_seed_ },
                { "network.carry_invariant", network_carry_invariant_ },
                { "network.phase_datum_eq34", network_phase_datum_eq34_ },
                { "network.mw_wl.ewl_enable", nc.mw_wl.ewl_enable } } },
            { network_phase_leveling_ || network_carry_invariant_,
              "zero-q phase memory + slip response",
              "STANDING INVARIANT #1: any stiff time-linked quantity MUST have a wired slip "
              "response before an ON field run — a stale value carried past a jump warps the "
              "whole solution (iono/clock absorb the tension)",
              { { "network.cycle_slip.mw_detect", network_cycle_slip_mw_detect_ },
                { "network.cycle_slip.if_phase_td", network_cycle_slip_if_phase_td_ } } },
            { network_raim_, "armed QC set",
              "a 96 km epoch entering the network is never acceptable; withhold is the "
              "backstop, adapt-and-emit is the goal",
              { { "network.raim_attribution", network_raim_attribution_ },
                { "network.raim_within_epoch", network_raim_within_epoch_ },
                { "network.clock_runaway_check", network_clock_runaway_check_ } } },
            { estimate_network_iono_sh_, "SH-VTEC / W6 slice",
              "the SH field is fed a per-epoch CODE-derived slant iono, so low-elevation code "
              "contamination dominates a00 — the 25 deg iono cutoff is load-bearing and is a "
              "SEPARATE gate from processing.elevation_mask_deg",
              { { "estimation.ionosphere_weighting.enable", iono_weighting_.enabled },
                { "estimation.ionosphere_weighting.elevation_cutoff_deg >= 25",
                  iono_weighting_.elevation_cutoff_deg >= 24.9 },
                // The bundle member is "the satellite code bias has a declared
                // source", not W6 in particular: EXT-CB satisfies it from the
                // external .BIA.  Naming only W6 here would log a spurious
                // partially-armed warning on every EXT-CB run.
                { "network.estimate_sat_code_bias | network.external_sat_code_bias",
                  estimate_network_sat_code_bias_ || network_external_sat_code_bias_ } } },
        };
    }

    /**
     * @brief Log, once per run, every knob the field record recommends for this run's regime
     *        and whether it is armed, plus the integrity of each coupled bundle.
     *
     * New work ships default-off and byte-identity-gated, and the project has no promotion
     * step, so validated options stay off unless a configuration arms them.  The loader
     * logs every knob it arms; this report logs the inverse, so the gap is visible at the
     * start of every run.
     *
     * Two design rules:
     *  - **Regime, not blanket.** A knob validated at one scale can be harmful at another
     *    (`mw_wl.bridge_sample_debias`: validated at 50 stations, net-harmful at 305, with
     *    the short-baseline wrong-integer rate going from 0.0 % to 12.5 %).  Every entry
     *    carries the regime it was validated in and is reported only when that regime
     *    matches this run.  That filter lives in @ref networkPostureRegime_ and in each
     *    row's `applies` predicate.
     *  - **Bundles, not knobs.** Several options are correct only when armed together;
     *    arming a strict subset is its own failure mode (the MW/WL lane stack without
     *    `cold_clock_seed` quarantines its lane cycles; `x_lin` measured 269 km at 305
     *    stations).  A partially armed bundle logs a warning; the table lives in
     *    @ref networkPostureBundles_.
     *
     * Log-only: reads already-parsed state and changes nothing.  The full evidence, and
     * the UNVALIDATED and KEEP_OFF tiers this report does not list, are in
     * `docs/CONFIG_promotion_ledger.md`; `scripts/config_posture_check.py` checks any
     * config offline.
     *
     * @param log  the "gnss_config" logger.
     */
    void GnssConfig::logNetworkPosture_(const std::shared_ptr<spdlog::logger>& log) const {
        using Tier = PostureTier;
        const auto&         nc    = network_config_;
        const PostureRegime r     = networkPostureRegime_();
        const std::size_t   n_sta = r.n_sta;
        const std::size_t   n_sys = r.n_sys;
        const bool          warm  = r.warm;
        const bool          lane  = r.lane;
        const bool          rt    = r.rt;

        // ON rows first, then SCOPED: the report is a single pass, so this append order is
        // what keeps the two blocks contiguous in the log.
        std::vector<PostureItem> items;
        appendNetworkPostureItemsOn_(r, items);
        appendNetworkPostureItemsScoped_(r, items);
        const std::vector<PostureBundle> bundles = networkPostureBundles_(r);

        log->info("=== Network POSTURE (regime: {} station(s), {} constellation(s), {} "
                  "frequency mode, {} start, {} lane layer, {}) — recommended-vs-armed; "
                  "evidence in docs/CONFIG_promotion_ledger.md ===",
                  n_sta, n_sys,
                  frequency_mode_ == FrequencyMode::All ? "all"
                      : frequency_mode_ == FrequencyMode::Triple ? "triple" : "dual",
                  warm ? "WARM-RESUME" : "cold",
                  lane ? "with" : "no",
                  rt ? "real-time/service" : "post-processed");
        int on_armed = 0, on_off = 0, sc_armed = 0, sc_off = 0, fp_blocked = 0;
        // ON-tier gaps log a warning: an ON knob has no known regime where it should stay
        // off.  SCOPED gaps log at info level: the table holds about 90 SCOPED rows, and
        // warnings on all of them would bury the ON tier.  Every gap is reported either way,
        // with the "POSTURE OFF" prefix.
        for (const auto& it : items) {
            if (!it.applies) continue;
            const bool on_tier = it.tier == Tier::ON;
            if (it.on) { (on_tier ? on_armed : sc_armed) += 1; continue; }
            (on_tier ? on_off : sc_off) += 1;
            if (on_tier) log->warn("  POSTURE OFF  [ON]     {} — {}", it.knob, it.why);
            else         log->info("  POSTURE OFF  [SCOPED] {} — {}", it.knob, it.why);
            // Chain check: on a warm resume a fingerprinted recommendation cannot be acted
            // on in this leg, since arming it breaks the lineage.  The note follows the
            // gap line and is always a warning, whatever the row's tier.
            if (warm && it.fp) {
                ++fp_blocked;
                log->warn("      ^ CHAIN: {} is in the warm-resume FINGERPRINT, so arming it on "
                          "this resumed leg {}.  Arm it in a FRESH lineage (new seed) instead — "
                          "a chain cannot adopt a fingerprinted knob mid-flight.",
                          it.knob,
                          nc.state_snapshot_require
                              ? "hard-aborts the leg (state_snapshot_require is on)"
                              : "SILENTLY cold-starts the leg — state_snapshot_require is FALSE, "
                                "so the fingerprint mismatch is never reported and the run looks "
                                "like a successful resume");
            }
        }
        if (fp_blocked > 0 && !nc.state_snapshot_require) {
            log->warn("  POSTURE CHAIN: {} fingerprinted recommendation(s) above, AND "
                      "network.state_snapshot_require is false — this is the configuration in "
                      "which acting on the advice fails SILENTLY (invariant #4: a silent "
                      "cold-start reads as a clean run).  Arm state_snapshot_require first; "
                      "then a mistaken flip aborts loudly instead of lying.", fp_blocked);
        }
        for (const auto& b : bundles) {
            if (!b.engaged) continue;
            std::string missing;
            int armed = 0;
            for (const auto& m : b.members) {
                if (m.on) { ++armed; continue; }
                if (!missing.empty()) missing += ", ";
                missing += m.knob;
            }
            if (armed == static_cast<int>(b.members.size())) {
                log->info("  POSTURE BUNDLE '{}': {}/{} armed — complete.",
                          b.name, armed, b.members.size());
            } else if (armed == 0) {
                log->info("  POSTURE BUNDLE '{}': engaged but 0/{} companion(s) armed — {}.  "
                          "Missing: {}", b.name, b.members.size(), b.rule, missing);
            } else {
                log->warn("  POSTURE BUNDLE '{}': PARTIAL {}/{} — a strict subset of a coupled "
                          "set is its own failure mode.  {}.  MISSING: {}",
                          b.name, armed, b.members.size(), b.rule, missing);
            }
        }
        log->info("=== Network POSTURE: ON tier {}/{} armed, SCOPED tier {}/{} armed "
                  "(regime-matched rows only; knobs validated in a DIFFERENT regime are "
                  "deliberately not listed).  Full evidence and the UNVALIDATED / KEEP_OFF "
                  "tiers: docs/CONFIG_promotion_ledger.md, or "
                  "`python scripts/config_posture_check.py <config> --tier all`{} ===",
                  on_armed, on_armed + on_off, sc_armed, sc_armed + sc_off,
                  fp_blocked > 0
                      ? fmt::format(".  FINGERPRINTED and needing a fresh lineage rather than "
                                    "this leg: {} row(s)", fp_blocked)
                      : std::string{});
    }

} // namespace GnssProcessor
