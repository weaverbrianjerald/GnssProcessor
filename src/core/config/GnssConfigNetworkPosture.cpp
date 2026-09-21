/*!
 * \file GnssConfigNetworkPosture.cpp
 * \brief The network POSTURE readout — recommended-vs-armed, per run regime.
 *
 * Split out of @c GnssConfigLoadNetwork.cpp (2026-08-01) in the same pass that
 * decomposed @c loadNetworkSection_.  Its own TU because it is the second
 * highest-churn surface in the config layer and it churns for a DIFFERENT
 * reason than the loader: a card that lands a knob edits the loader, and a card
 * that VALIDATES a knob adds a row here.  Two concurrent sessions doing those
 * two things no longer touch one file.
 *
 * The readout is log-only: it reads already-parsed state and changes nothing.
 * It is split four ways so that a new recommendation is a one-row edit inside
 * exactly one function:
 *
 *   - @c networkPostureRegime_          — the regime predicates every row filters on
 *   - @c appendNetworkPostureItemsOn_    — the ON tier rows      (gaps WARN)
 *   - @c appendNetworkPostureItemsScoped_ — the SCOPED tier rows (gaps INFO)
 *   - @c networkPostureBundles_          — the COUPLED bundles   (partial arm WARNs)
 *   - @c logNetworkPosture_              — the report pass over all of the above
 *
 * The ON-then-SCOPED append order in @c logNetworkPosture_ is what keeps the two
 * blocks contiguous in the log; the old single table relied on a hand-sorted
 * initializer list and a comment to say so.
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
    //  POSTURE readout — what the field record recommends vs what is armed
    // =================================================================
    /**
     * @brief Compute the regime predicates every posture row filters on.
     *
     * A knob validated at one scale can be harmful at another
     * (@c mw_wl.bridge_sample_debias: validated at 50 stations, net-harmful at 305 —
     * short-baseline wrong-integer rate 0.0 % -> 12.5 %), so no row is reported unless
     * the regime it was validated in matches THIS run.  Computed once and handed to
     * every table builder.
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
        const bool lane  = nc.mw_wl.ewl_enable || nc.mw_wl.enable || nc.lane_carry.enable;
        // The real-time / service posture.  service_mode IS the RT service producer, so a
        // knob scoped "post-processing only" (carry_invariant: ~57% of epoch wall time at
        // 305 sta) must stay silent there, and the RT-only knobs must stay silent everywhere
        // else -- that asymmetry is the whole point of regime filtering.
        const bool rt    = app_mode_ == AppMode::RealTime || network_service_mode_;
        // The per-station parallel family is SILENTLY inert below the threshold
        // (ApplicationNetworkSetup.cpp:862-865), so seam/carry are only worth reporting
        // where the family actually engages.
        const bool par   = nc.parallel_stations
                        && static_cast<int>(n_sta) > nc.parallel_min_stations;
        // Config-determinable epoch spacing, same derivation the emit_clock_rate inert-arming
        // guard uses at :1749.  0 when gnss_rate_s is 0 (native RINEX rate, unknown at load).
        const double spacing_s = gnss_rate_s_ * static_cast<double>(nc.epoch_decimation_n);
        // active_systems_ is a vector of NAMES, so a COUNT does not identify a constellation
        // set: n_sys <= 2 admits GPS+BeiDou, which config/test_suite/_ppprtk50_4_gps_bds.json
        // is today and which the W6 code-bias scope explicitly excludes.  Test the names.
        const bool gps_only = n_sys == 1 && active_systems_.front() == "GPS";
        const bool gps_gal_only =
            !active_systems_.empty()
            && std::all_of(active_systems_.begin(), active_systems_.end(),
                           [](const std::string& s) { return s == "GPS" || s == "Galileo"; });

        return PostureRegime{ n_sta, n_sys, tri, warm, lane, rt, par,
                              spacing_s, gps_only, gps_gal_only };
    }

    /**
     * @brief Append the ON-tier recommendation rows (a regime-matched gap WARNs).
     *
     * ON tier = no known counter-scope in this run's regime.  These are exactly what
     * gets re-discovered by accident one wasted run at a time, which is why their gaps
     * warn while the 40+ SCOPED gaps only inform.
     *
     * @param r    Regime facts from @ref networkPostureRegime_.
     * @param out  Row sink; appended to (call this BEFORE the SCOPED builder so the two
     *             blocks stay contiguous in the log).
     */
    void GnssConfig::appendNetworkPostureItemsOn_(const PostureRegime& r,
                                                  std::vector<PostureItem>& out) const {
        // Alias preamble: the rows below are the original table text, unedited.
        using Tier = PostureTier;
        using Item = PostureItem;
        const auto&       nc    = network_config_;
        const std::size_t n_sta = r.n_sta;
        const bool        tri   = r.tri;
        const bool        warm  = r.warm;
        const bool        par   = r.par;

        const std::vector<Item> rows = {
            // --- ON tier: Card-4 invariant DD carry, its carry couplings --------------
            // 2026-09-05: NOT reported on the phase-leveling F1/F3 lineages — the two carry
            // VEHICLES are mutually exclusive (do_amb_carry skips the leveling inject under
            // carry_invariant, so F3 would be inert there; SPEC_amb_iono_split_overcount).
            { !network_phase_leveling_local_block_ && !network_phase_leveling_los_scale_,
              network_carry_invariant_,
              Tier::ON, true , "network.carry_invariant",
              "the ONLY cross-epoch phase memory a per-epoch estimator has; consumer 3D float "
              "10.9 -> 3.6 m" },
            { network_carry_invariant_innov_gate_,
              network_carry_invariant_innov_calibrate_,
              Tier::ON, false, "network.carry_invariant_innov_calibrate",
              "raw formal Cov(v) is 10-30x optimistic (M2); calibrated k=4 reproduces the "
              "hand-tuned k=10 sweet spot" },
            { network_carry_invariant_,
              network_carry_invariant_innov_gate_,
              Tier::ON, false, "network.carry_invariant_innov_gate",
              "invariant #1 event response for the stiff DD carry; em10 1h-tail DD step 8.33 -> "
              "2.35 cyc/ep" },
            { network_carry_invariant_innov_gate_,
              network_carry_invariant_innov_calibrate_ ? (network_carry_invariant_innov_k_ >= 3.99 &&
                  network_carry_invariant_innov_k_ <= 30.01) : (network_carry_invariant_innov_k_ >= 9.99),
              Tier::ON, false, "network.carry_invariant_innov_k",
              "k=4 (sweep flat over 4-30) is right ONLY with innov_calibrate; on raw formal "
              "Cov(v) the field value is k=10" },
            { network_carry_invariant_,
              network_carry_invariant_qz_ >= 5e-5 && network_carry_invariant_qz_ <= 2e-4,
              Tier::ON, false, "network.carry_invariant_qz",
              "set 1e-4 m2/s: beats zero-q, DD step 2.433 -> 1.498 cyc/ep and calib 0.82 -> 0.49 "
              "at 50 sta 3h" },
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
            // --- ON tier: QC, RAIM and SLIP knobs -------------------------------------
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
            // --- s22 THE REFERENCE-ARC CLAUSE (promoted 2026-09-12): a silent MW pair whose
            //     IF-TD sample is the chain's REFERENCE satellite is UNTESTED, not clean (dd == 0
            //     by construction, so the pooled preview can never flag it and the veto is
            //     structurally unavailable).  A clause of the scope's rule: regime = the scope.
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
            // --- ON tier: EMISSION and PRODUCT knobs ----------------------------------
            { network_emit_pb_filter_ && network_carry_invariant_ && network_carry_invariant_emit_rebase_,
              network_emit_pb_filter_gate_v2_,
              Tier::ON, false, "network.emit_pb_filter_gate_v2",
              "G1-G5 all PASS: disc floor 2.6/ep vs v1 85.5/ep (33x), innovation resets 5707 -> "
              "32 (178x), solve byte-identical" },
            { true,
              network_phase_leveling_,
              Tier::ON, true , "network.phase_leveling",
              "armed in 107 of 108 shipped configs; zero-q sharpens clock 2.44 -> 2.00 m (-18%); "
              "the carry_warm gate for RAIM" },
            // B6 S-basis contract line (ledger §A2 F16, APPLIED into the goldens 2026-08-08):
            // header-only, solve-byte-identical; replay consumer -12.7 % RMS, 45 spurious
            // resets removed.  Was a ledger row with no loader row until 2026-09-05.
            // RTA-10 H heartbeat record (ledger §A2 F16, APPLIED with h_sigma0_prefit_census):
            // record stream 0-diff, solve-neutral; a tail consumer distinguishes "withheld"
            // from "producer died".  Was a ledger row with no loader row until 2026-09-05.
            { true,
              network_emit_epoch_status_record_,
              Tier::ON, false, "network.emit_epoch_status_record",
              "one positional H heartbeat record per epoch incl. withheld ones (gate + sigma0): "
              "record stream 0-diff, solve-neutral; pair with h_sigma0_prefit_census" },
            { true,
              nc.serialize_sbasis_contract,
              Tier::ON, false, "network.serialize_sbasis_contract",
              "stamp the S-basis contract into the product header: replay consumer -12.7 % RMS, "
              "45 spurious resets removed; header-only, solve byte-identical" },
            { true,
              nc.write_run_manifest,
              Tier::ON, false, "network.write_run_manifest",
              "run provenance: config hash + build stamp + POST-arming toggle dump; no estimation "
              "effect, no product bytes" },
            // --- ON tier: IONOSPHERE and SH-VTEC --------------------------------------
            { n_sta >= 3 && tri,
              estimate_network_iono_sh_,
              Tier::ON, true , "network.estimate_iono_sh",
              "USER DIRECTIVE: always on; without SH consumer float 8.19 -> 0.98 m and robust "
              "clock spread 266 m vs single digits" },
            // --- ON tier: PERFORMANCE / PARALLELISM + network trace_* knobs -----------
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
            // --- ON tier: COLD-START, DATUM and CLOCK knobs ---------------------------
            { network_phase_datum_eq34_ || network_phase_datum_prioritized_,
              network_station_pd_pin_aware_,
              Tier::ON, false, "network.station_pd_pin_aware",
              "305-sta 3h: 884 spurious pin-driven station rejections -> 0; clock common-mode "
              "0.524 -> 0.376" },
        };
        out.insert(out.end(), rows.begin(), rows.end());
    }

    /**
     * @brief Append the SCOPED-tier recommendation rows (a regime-matched gap INFOs).
     *
     * SCOPED tier = validated, but only inside a stated scope (a station count, a
     * constellation set, a lineage shape).  There are 40+ of them; warning on all would
     * drown the ON tier, so they inform and stay one grep away.
     *
     * @param r    Regime facts from @ref networkPostureRegime_.
     * @param out  Row sink; appended to (call this AFTER the ON builder).
     */
    void GnssConfig::appendNetworkPostureItemsScoped_(const PostureRegime& r,
                                                      std::vector<PostureItem>& out) const {
        // Alias preamble: the rows below are the original table text, unedited.
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
            // --- SCOPED tier: Card-4 invariant DD carry, its carry couplings ----------
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
            { !network_carry_invariant_ && network_phase_leveling_,
              network_shared_carry_full_cov_,
              Tier::SCOPED, false, "network.shared_carry_full_cov",
              "legacy lineage: recovers the clock-to-phase-bias off-diagonals the diagonal stash "
              "drops; 50 sta cost ~0" },
            { warm && network_carry_invariant_ && gps_only,
              nc.state_snapshot_carry,
              Tier::SCOPED, false, "network.state_snapshot_carry",
              "GPS-only warm resume: CLK slope 55.8 -> 30.2 mm/ep over 60 ep; GPS+Gal at 305 sta "
              "is NEGATIVE, unresolved" },
            { nc.state_snapshot_carry && gps_only,
              nc.state_snapshot_carry_diagonal,
              Tier::SCOPED, false, "network.state_snapshot_carry_diagonal",
              "GPS-only: accuracy >= dense at 478x smaller (270.9 -> 0.57 MB at 305 sta); dense "
              "churns ~186 GB per 6h" },
            // --- RS-2 (SPEC_event_harness s21, 2026-09-12): the WRITE half of the
            //     linearization-frame carry.  The blob's Z rows are correction-frame DDs;
            //     without the writer's sum c*x_lin per chord a resumed run folds every
            //     restored chord in the wrong frame.  Write side only (products byte-identical,
            //     only the snapshot grows); regime = a carry blob is being written at all.
            { nc.state_snapshot_carry,
              nc.state_snapshot_carry_lin,
              Tier::SCOPED, false, "network.state_snapshot_carry_lin",
              "write one ZL row (sum c*x_lin per chord) per carried block so a warm resume can "
              "re-base the restored branch onto its own linearization (network.resume_carry_rebase); "
              "the field golden's warm resume went from 61/61 withheld to emitting (SPEC_event_harness "
              "s21.5)" },
            // --- RS-3 (SPEC_event_harness s21.7): WRITE half of datum continuity - the eq34
            //     chain tree and the time-linked code-bias datum memory ride the snapshot as
            //     format-forward rows.  Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty()
                  && (network_phase_datum_eq34_ || network_code_datum_time_linked_),
              nc.state_snapshot_datum_capture,
              Tier::SCOPED, false, "network.state_snapshot_datum_capture",
              "carry the eq34 chain tree + the time-linked code-bias datum memory in the snapshot so a "
              "warm resume can continue both datums (RS-3): first-epoch eq34 re-references 110 -> 8, "
              "code-bias re-seed discontinuities 30 -> 0, R record 30 -> 0 satellites" },
            // --- RS-5 (SPEC_event_harness s21.10/s21.11): WRITE half of emit-layer continuity -
            //     the eq34 emit re-base frame and the PB emission filter state ride the snapshot.
            //     Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty()
                  && (network_carry_invariant_emit_rebase_ || network_emit_pb_filter_),
              nc.state_snapshot_emit_capture,
              Tier::SCOPED, false, "network.state_snapshot_emit_capture",
              "carry the emit layer's memory (ER/PF/PS rows) so a warm resume does not cold-start the "
              "eq34 emit re-base and the PB filter: the seam's 17.7 m per-key phase-bias step becomes "
              "0.000 m p50 (RS-5)" },
            // --- RS-6a (SPEC_event_harness s21.13/s21.14): WRITE half of station-layer
            //     continuity - the rx-CB carry and the T5 per-LOS deviation carry ride the
            //     snapshot (RC/SD rows).  Write side only, products byte-identical.
            { !nc.state_snapshot_path.empty(),
              nc.state_snapshot_station_capture,
              Tier::SCOPED, false, "network.state_snapshot_station_capture",
              "carry the rx-CB carry + the T5 iono deviation carry in the snapshot so a warm resume "
              "continues the station layer's memory: the SH a00 seam transient +4.6 -> +0.4 TECU, "
              "receiver clocks at +1 1.21 -> 0.91 m (RS-6a)" },
            // --- RS-9 (SPEC_event_harness s21.18/s21.19): WRITE half - the rx-CB carry's
            //     linearization point (RL rows beside RC) rides the snapshot.  Write side only,
            //     products byte-identical; requires state_snapshot_station_capture.
            { !nc.state_snapshot_path.empty() && nc.state_snapshot_station_capture,
              nc.state_snapshot_rxcb_lin_capture,
              Tier::SCOPED, false, "network.state_snapshot_rxcb_lin_capture",
              "carry the rx-CB carry's code-bias linearization point so a warm resume judges the "
              "restored carry against the writer's frame, not one epoch of resume code (RS-9)" },
            // --- RS-8 (SPEC_event_harness s21.16/s21.20): WRITE half - every station's locked
            //     canonical-signal map (CN rows) rides the snapshot.  Write side only, products
            //     byte-identical.
            { !nc.state_snapshot_path.empty(),
              nc.state_snapshot_signal_capture,
              Tier::SCOPED, false, "network.state_snapshot_signal_capture",
              "carry each station's canonical-signal map so a warm resume does not re-decide 23 of "
              "61 stations' channels from the resume epoch: chord births at the seam 64 -> 1 (RS-8)" },
            // RS-10 (SPEC_event_harness s21.21): only where the lineage CARRIES the ZWD (the F3
            // station block or carry_zwd).  On a white-sink ZWD (the field golden) the B9 prior
            // is ~4000x too stiff and the harvest turns it into the falsified diagonal carry.
            { warm && (network_phase_leveling_local_block_ || network_carry_zwd_),
              network_zwd_seam_reseed_,
              Tier::SCOPED, false, "network.zwd_seam_reseed",
              "warm-resume ZWD no-restart (h6 truth rms 0.124 vs cold 0.390); settled h9-12 "
              "consumer 0.134 vs no-Z warm 0.278; NOT fingerprinted; OFF where the lineage carries "
              "no ZWD (RS-10: zwd prior sigma0 11, ZWD leg-seed 0.29 -> 1.34 m by +60)" },
            // --- RS-10b (SPEC_event_harness s21.21): the seam reseed's own stiffness, seam-scoped
            //     and fingerprint-exempt (it borrowed the fingerprinted zwd_carry_q before).
            { warm && network_zwd_seam_reseed_,
              network_zwd_seam_reseed_q_ > 0.0,
              Tier::SCOPED, false, "network.zwd_seam_reseed_q",
              "give the ZWD seam reseed its own stiffness: at the inherited 1e-9 the seam prior is ~4000x "
              "too stiff on a white-sink ZWD (sigma0 11, ZWD leg-seed 1.34 m by +60); tuned, the tails "
              "collapse to the reseed-OFF arm's while the cold consumer keeps its smoother start (RS-10b)" },
            // --- SCOPED tier: QC, RAIM and SLIP knobs ---------------------------------
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
            // --- s22.6/s22.7 REF-FAN (2026-09-13): the reference arm on the ADDITIONAL chains, so a
            //     slip ON a chain's own reference stops fanning out; own-elected references through
            //     the empirical gate, adopted ones inherit the first chain's verdict.  Regime = pairs
            //     all + the empirical gate.
            { nc.cycle_slip_if_phase_td_pairs == NetworkIfPhaseTdPairs::All && nc.slip_qc.ref_gate,
              nc.slip_qc.ref_arm_all,
              Tier::SCOPED, false, "network.cycle_slip.if_phase_td_ref_arm_all",
              "run the reference arm on the additional IF-TD chains (empirical gate): collateral refloats "
              "6 -> 0 on every GOPE/G24 event, the 13.3 m innocent PB re-realization removed, consumer mean "
              "1.4-2.7x better, flood held (93/94 wide-chain candidates suppressed); sim storm arm owed" },
            // --- Card G RESPONSE on the FLOAT vehicle (AB2 sections 12 to 15, ledger
            //     amended 2026-09-20).  Freeing a re-entering arc makes it re-level from ONE
            //     epoch of code, which bought a KEEP_OFF on the converging LEVELING vehicle
            //     (float consumer +56 % in hours 3 to 9) and is priced differently by a float
            //     ambiguity state with a floored birth prior.  Regime = the vehicle; on the
            //     leveling one the KEEP_OFF still stands, so this row must NOT report there.
            { nc.ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf,
              nc.slip_qc.gap_reentry,
              Tier::SCOPED, false, "network.cycle_slip.gap_reentry_relevel",
              "free a re-entering arc's ambiguities on the float vehicle: 12 h field post-fit "
              "refloats 1069 -> 702, epochs below a decimetre 223 -> 321, clock wire on the "
              "consumer's satellites 0.0406 -> 0.0203 m, never worse than 0.03 m of consumer "
              "p50 in hours 3 to 12; needs cycle_slip.relevel_free_epoch to act" },
            { true,
              network_exclude_unmodeled_antenna_stations_,
              Tier::SCOPED, false, "network.exclude_unmodeled_antenna_stations",
              "backstop: an unmodelled receiver PCO/PCV is a systematic in every obs feeding the "
              "shared clocks and biases" },
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
            // --- RAIM pool re-host (docs/SPEC_raim_pool_rehost.md s7.1, 2026-09-13): the post-fit
            //     screen runs on carry_invariant lineages (qc_amb census); neutral until it sees
            //     something.  The actuator stays a separate KEEP_OFF knob.
            { network_carry_invariant_ && network_raim_,
              network_raim_carry_invariant_,
              Tier::SCOPED, false, "network.raim_carry_invariant",
              "re-host the post-fit RAIM pool onto the carry-neutral qc_amb census so T3/T4 run at all "
              "under carry_invariant: 107k cells / 118 epochs on the field golden, products byte-identical, "
              "~5 % epoch cost; sigma0 at its 1.0 floor so far (power leg owed)" },
            // --- SCOPED tier: EMISSION and PRODUCT knobs ------------------------------
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
            { network_carry_invariant_ && network_carry_invariant_emit_rebase_,
              network_emit_pb_filter_,
              Tier::SCOPED, false, "network.emit_pb_filter",
              "clears the ~200/ep PB disc floor (median step 0.080 m vs a 0.05 m thresh; 50 of "
              "11938 genuine); solve unchanged" },
            { network_emit_pb_filter_ && network_carry_invariant_ && network_carry_invariant_emit_rebase_
                  && network_emit_pb_filter_gate_v2_,
              network_emit_pb_filter_jump_k_ > 3.99 && network_emit_pb_filter_jump_k_ < 4.01,
              Tier::SCOPED, false, "network.emit_pb_filter_jump_k",
              "keep the k=4.0 default under gate_v2 - one k transfers 30 s to 300 s; jump_k=8 is "
              "the RETIRED v1 hand tuning" },
            { network_emit_pb_filter_ && network_carry_invariant_ && network_carry_invariant_emit_rebase_
                  && (nc.epoch_decimation_n > 1 || spacing_s > 60.0),
              network_emit_pb_filter_window_epochs_ < 59.99 || network_emit_pb_filter_window_epochs_ >
                  60.01,
              Tier::SCOPED, false, "network.emit_pb_filter_window_epochs",
              "epoch-denominated: the 60.0 default does NOT transfer across cadence; the shipped "
              "300 s tier uses 15.0" },
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
            // --- SCOPED tier: IONOSPHERE and SH-VTEC ----------------------------------
            { estimate_network_iono_sh_ && n_sta >= 150 && iono_weighting_.elevation_cutoff_deg >= 24.9,
              global_vtec_sh_.max_degree >= 15,
              Tier::SCOPED, false, "estimation.global_vtec_sh.max_degree",
              "deg-15 at >=150 sta: sat code-OSB drift 0.66 vs 2.31 m, epoch step 1.57 vs 4.43 "
              "cm; only at a 25 deg iono cutoff" },
            { estimate_network_iono_sh_ && n_sta >= 50,
              iono_weighting_.elevation_cutoff_deg >= 24.9,
              Tier::SCOPED, false, "estimation.ionosphere_weighting.elevation_cutoff_deg",
              "25 deg cuts clock-delta rms 77x vs 10 deg (a00 296 -> -305 TECU); SEPARATE gate "
              "from processing.elevation_mask_deg" },
            { estimate_network_iono_sh_ && n_sta >= 50,
              iono_constraint_deweight_.adaptive,
              Tier::SCOPED, false, "network.iono_constraint_deweight_adaptive",
              "robust-z deweight of a hostile iono minority; 2-sta amputation cut ignition 5-9 -> "
              "0.88 cyc/ep; own A/B never run" },
            { estimate_network_iono_sh_ && iono_constraint_deweight_.adaptive,
              iono_constraint_deweight_.min_epochs <= 5,
              Tier::SCOPED, false, "network.iono_constraint_deweight_min_epochs",
              "field legs all run 5 clean samples, not the shipped 30 (6x slower gate ignition); "
              "epoch-denominated, never rescale" },
            { estimate_network_iono_sh_ && iono_constraint_deweight_.adaptive,
              iono_constraint_deweight_.window_epochs <= 15.01,
              Tier::SCOPED, false, "network.iono_constraint_deweight_window_epochs",
              "field runs use a 15-epoch robust-z half-life; shipped 60 is an un-retuned 30 s "
              "assumption (Class A, scales w/ rate)" },
            // --- SCOPED tier: PERFORMANCE / PARALLELISM + network trace_* knobs -------
            { true,
              nc.health_interval_s > 0.0,
              Tier::SCOPED, false, "network.health_interval_s",
              "set 60-300 s: the ONLY mid-run-flushed CSV, so a crashed or WER-zombied run loses "
              "every other tail" },
            { nc.lane_carry.enable && par,
              nc.lane_carry.parallel_components,
              Tier::SCOPED, false, "network.lane_carry.parallel_components",
              "the one lane knob whose ON path is byte-identical (independent joint blocks, Eigen "
              "re-pinned to 1)" },
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
            { estimate_network_iono_sh_ && n_sta <= 50,
              nc.trace_ionosphere,
              Tier::SCOPED, false, "network.trace_ionosphere",
              "the pierce-point / slant-iono viz record; a full extra reconstruct sweep, so KEEP "
              "OFF above ~50 sta.  Grade iono_total_m, NEVER iono_m (the latter is the seed-less "
              "correction: median 0.95 m against a true 8.88 m)" },
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
            // --- SCOPED tier: COLD-START, DATUM and CLOCK knobs -----------------------
            { rt && !(estimate_network_sat_code_bias_ && !estimate_network_iono_sh_),
              network_apriori_source_ == NetworkAprioriSource::BrdcRts,
              Tier::SCOPED, true , "network.apriori_source",
              "self-sufficient RT service needs apriori_source=brdc_rts; float 2.84 m vs sp3 2.76 "
              "m (3 sta/121 ep)" },
            { estimate_network_sat_code_bias_ && gps_gal_only,
              network_cb_kaula_prior_,
              Tier::SCOPED, false, "network.cb_kaula_prior",
              "lifts the CODE-dominated near-null direction the PA heal cannot reach (it anchors "
              "PB only); FT63 GPS+Gal 145/150 -> 150/150 emitted" },
            { network_cb_kaula_prior_,
              network_cb_kaula_sigma_m_ <= 3.01,
              Tier::SCOPED, false, "network.cb_kaula_sigma_m",
              "sigma=3 m gives 2.7 decades of K2 margin; the 10 m default gives 1.6 and survives "
              "only by coincidence" },
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
            { lane,
              network_cold_clock_seed_,
              Tier::SCOPED, false, "network.cold_clock_seed",
              "lane/WL layers difference ~3e5-magnitude x_lin; measured 269 km at 305 sta cold" },
            { n_sta >= 3 && gps_gal_only,
              estimate_network_sat_code_bias_,
              Tier::SCOPED, true , "network.estimate_sat_code_bias",
              "W6 OSB is a drop-in for the .BIA product (3-sta closed loop 0.508 m vs 0.489 m "
              "3D); grade BIAS-ONLY" },
            { network_carry_invariant_,
              network_phase_datum_eq34_,
              Tier::SCOPED, true , "network.phase_datum_eq34",
              "the Card-4 DD datum; arm ONLY with carry_invariant - eq34 alone measured 76.2 vs "
              "0.085 cyc/ep DD-step" },
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
            { network_phase_datum_prioritized_ && !network_phase_datum_eq34_,
              network_phase_anchor_carry_reref_,
              Tier::SCOPED, false, "network.phase_anchor_carry_reref",
              "QUEUE 1(d): the PHASE_ANCHOR handover re-realizes the datum on the design matrix "
              "ONLY, so the carry folds in the RETIRED frame and the clock absorbs it (1e10:1). "
              "24-sta/1h/GPS sim: clock episodes 7 -> 1, sum|dev| 160.5 -> 1.5 m, datum-free C "
              "contrast 18.66 -> 0.50 m. n=1, field arm OWED; eq34 owns eq34_reref instead" },
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
            // PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md): the fix for the corrupt
            // carry substrate — PD gate 87.7 % failing -> 0 on the 3 h field leg.  Only where the
            // Card-4 carry exists; fingerprinted ('carry:pdsafe').
            { network_carry_invariant_,
              network_carry_invariant_pd_safe_,
              Tier::SCOPED, true , "network.carry_invariant_pd_safe",
              "PD-safe fold of the invariant DD carry: PD gate 686/782 failing -> 0/782, mature "
              "DD steps 2.75 -> 1.02 cyc/ep (fresh-convergence lineages only)" },
            // RTA-1 sticky clock-pivot election (STABILITY E3, SPEC_synthetic_minimum_network
            // §18.5/§18.8): recommended where per-satellite re-realizations at re-admission are
            // material — triple-frequency / weak-third-signal postures (settled 24 h consumer
            // 3D rms 0.303 -> 0.078 m on the sim bed).  GPS-dual+SH field: mechanism transfers,
            // harm absent, so not evidence-recommended there.  Inert under the anchor-free
            // clock datums.  Fingerprinted (fresh lineage only).
            // --- PIVOT DEPARTURE (SPEC_event_harness §19, promoted 2026-09-11).  A pivot
            //     handover's carry skip / oscillator clear / jump-test suppression is
            //     applied NETWORK-WIDE by default, so one constellation's datum change
            //     re-realizes the other's carry — producer-internal free-fall, not a datum
            //     move the consumer could transform away.  Needs a pivot datum (inert
            //     under the anchor-free ones) and a second constellation to spare.
            //     Fingerprinted: fresh-convergence only, no warm-resume A/B exists.
            { r.n_sys >= 2 && !network_clock_datum_sum_to_zero_
                  && !network_clock_datum_broadcast_anchored_,
              network_scoped_handover_response_,
              Tier::ON, true , "network.scoped_handover_response",
              "scope the pivot-handover carry skip to the constellation whose datum changed: "
              "collateral re-realization is the user's metre-class shock (storm arm settled "
              "p50 0.1204 -> 0.0825 m, max |3D| 1.34 -> 0.20, hour 5 0.236 -> 0.047; fresh "
              "pair edge 1.143 -> 0.117 m) while the producer's own contrasts move < 0.5 % "
              "(runs/golden_sim_campaign_zwdblk_scoped, exe F4FF8EF0)" },
            // --- PIVOT DEPARTURE increment 1 (SPEC_event_harness §19.6, promoted
            //     2026-09-12).  Increment 0 above stops a handover invalidating the
            //     constellations whose datum did NOT move; this re-expresses the CHANGED
            //     constellation's own carry along the structural clock null direction and
            //     FOLDS it, instead of dropping it for one epoch.  Needs a pivot datum (no
            //     handover seam without one) and its own prerequisite; no n_sys term —
            //     it acts on the constellation that changed, so one is enough.
            //     Fingerprinted (armed-only token): fresh-convergence only.
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
            { r.tri && !network_clock_datum_sum_to_zero_ && !network_clock_datum_broadcast_anchored_,
              network_sticky_pivot_,
              Tier::SCOPED, true , "network.sticky_pivot",
              "sticky clock-pivot election: 8 -> 2 pivot handovers/day, the h16-h18 pivot-eclipse "
              "consumer excursion eliminated (settled 24 h 3D rms 0.303 -> 0.078 m); "
              "triple-freq / weak-third-signal postures" },
            // --- SCOPED tier: the phase-leveling carry over-count fixes (2026-09-04/05,
            //     SPEC_amb_iono_split_overcount).  Only where the leveling carry is the live
            //     vehicle (phase_leveling on, carry_invariant off, Card-1 coupling rows on,
            //     Card-1b joint whitening off).  Both fingerprinted ('pl:losblock' /
            //     'pl:losscale').  F3 supersedes F1, so F1 is reported only without F3.
            { network_phase_leveling_ && !network_carry_invariant_
                  && network_carry_conditional_coupling_ && !network_carry_coupling_joint_whiten_,
              network_phase_leveling_local_block_,
              Tier::SCOPED, true , "network.phase_leveling_local_block",
              "F3 station-wide whitened local block: removes the B-fold per-LOS and the "
              "~n_LOS-fold station-common over-count of the diagonal leveling carry — settled DD "
              "2.72 -> 0.22 cyc, WL 36 -> 99 % at the true integer (sim 3 h); pair with "
              "phase_datum_handover_max_var_m2 >= 1e-2" },
            // 2026-09-10 (SPEC_amb_iono_split_overcount §6.16.7d, arms Q/Z/F/F2): reported ONLY
            // where the ZWD carry is actually armed on an F3 lineage — off either half there is
            // no diagonal row to replace, and the loader interlock has already spoken.
            { network_carry_zwd_ && network_phase_leveling_local_block_,
              network_carry_zwd_in_block_,
              Tier::SCOPED, true , "network.carry_zwd_in_block",
              "MANDATORY with carry_zwd under F3: the diagonal ZWD row is the same station-common "
              "double count F3 exists to remove — quiet sim consumer 3D rms 0.723 m diagonal vs "
              "0.0877 m in-block, field T3c 7619 vs 849 flags; field F3 consumer 3D -27 %, "
              "vertical DC bias +2.83 -> +0.07 m" },
            { network_phase_leveling_ && !network_carry_invariant_
                  && !network_phase_leveling_local_block_,
              network_phase_leveling_los_scale_,
              Tier::SCOPED, true , "network.phase_leveling_los_scale",
              "F1 per-LOS B_eff variance scale: undoes the B-fold iono-direction over-count "
              "(settled DD 2.72 -> 1.28 cyc); SUPERSEDED by phase_leveling_local_block" },
            { network_phase_leveling_local_block_ || network_phase_leveling_los_scale_,
              network_phase_datum_handover_max_var_m2_ >= 0.0099,
              Tier::SCOPED, false, "network.phase_datum_handover_max_var_m2",
              "under F1/F3's honest sigma the lineage's 1e-4 m^2 PB-gauge handover ceiling refuses "
              "93/93 successors (cold handovers, 4-14 m PB frame steps); 1e-2 restores 70/93 and "
              "the settled window — set >= 1e-2 (a working value, not a promotion)" },
            // E2 seed-first re-admission (SPEC_synthetic_minimum_network §18.13/§18.14,
            // SPEC_amb_iono_split_overcount §6.10/§6.11, 2026-09-05): a departing satellite's D2
            // clock + code-bias carry is PARKED and seeds its re-registered columns, so the Q3
            // anchor folds over the true gap instead of tripping `gap` and freezing a code-level
            // re-draw into a metre-class tail.  Validated on the 24 h sim bed (T3 tails G25 6.27
            // -> 1.69 m, G12 5.38 -> 2.71, G26 6.51 -> 4.34; consumer post-eclipse excursion 2-3x
            // smaller; DD unchanged); the clock-only variant (readmit_seed_code_bias false) is
            // FALSIFIED (the tails return), so both kinds seed.  Q3-anchored lineages without
            // clock_model.  Not fingerprinted.  (readmit_coupling_marg_ep and
            // phase_datum_handover_max_var_ratio stay KEEP_OFF and have no posture row.)
            // Resume FRAME + COUPLING-REGRESSOR restore (SPEC_synthetic_minimum_network
            // §18.10–§18.14, 2026-09-06): on a warm resume, restore the phase-anchor incumbents +
            // sticky targets, the Q3 per-sat anchor state, the CLK-R held frame, every restored
            // leveling row's Card-1 regression AND the dense D2 border covariance (QK/QP), so the
            // first warm fold is conditional and dense.  Leg E: consumer h12 0.62 m rms (p50 0.10)
            // vs 2.05 with the frame + regressors alone and 0.52 for a cold consumer on the
            // continuous wire; first-fold clock trips 33 → 1.  Warm-resume lineages only (a
            // writer armed with state_snapshot_frame_capture + state_snapshot_coupling_capture).
            // Forward-only resume policies, not fingerprinted.
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
            // --- RS-2 (SPEC_event_harness s21.4): the READ half.  At the first post-resume DD
            //     prior build z_hat += W*zl_writer - zl_now (W = the cycle-basis projection the
            //     build already forms; Pz untouched).  Forward-only resume policy, not
            //     fingerprinted; a blob without ZL rows warns and keeps today's behaviour.
            { warm && nc.state_snapshot_carry,
              network_resume_carry_rebase_,
              Tier::SCOPED, false, "network.resume_carry_rebase",
              "re-base the restored invariant DD branch onto this run's linearization at the first "
              "post-resume build instead of folding it in the writer's frame (the field golden's "
              "61/61-withheld resume: dd_fold 7.8e7 of RHS norm, 676 D5 flags at +1; SPEC_event_harness "
              "s21.5)" },
            // --- RS-2b (SPEC_event_harness s21.6/s21.7): the D5 gate is DIAGNOSTIC-ONLY for K
            //     post-resume epochs so the restored (re-based) branch is not erased by the
            //     cold station layer's one-epoch disagreement (305 -> 7 rebirths at +2; the
            //     gate runs at the seed's own rate afterwards).  Forward-only resume policy.
            { warm && nc.state_snapshot_carry,
              network_resume_d5_warmup_ep_ > 0,
              Tier::SCOPED, false, "network.resume_d5_warmup_ep",
              "hold the restored invariant DD branch through the first K post-resume epochs "
              "(D5 trips counted, not re-tapped; K = 1 suffices): +2 chord rebirths 305 -> 7, live "
              "D5 trips afterwards 0-2 per epoch = the seed's own rate (SPEC_event_harness s21.7)" },
            // --- RS-3 (SPEC_event_harness s21.7): READ half - continue the writer's eq34 tree
            //     and code-bias datum at the seam instead of re-electing / re-seeding them.
            //     Forward-only resume policy, not fingerprinted; requires resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_datum_restore_,
              Tier::SCOPED, false, "network.resume_datum_restore",
              "restore the eq34 tree maps and the code-bias datum memory at a warm resume (RS-3): "
              "eq34 rerefs at +1 110 -> 8, CB re-seed discs 30 -> 0, R record 30 -> 0 sats; a ~10 m "
              "phase-bias re-realization remains and is the next card (SPEC_event_harness s21.9)" },
            // --- RS-4 (SPEC_event_harness s21.9): the DECLARATION half of the same seam - the
            //     first post-resume emit marks datum_changed for any constellation whose
            //     phase-bias level re-realized against the RESTORED emitter baselines.  The
            //     CLK-R resume_seam rule cannot see it (it fires only on a COLD held-frame
            //     engine, i.e. never on a lineage that restores the frame).  Outputs-only,
            //     forward-only resume response policy, not fingerprinted.
            { warm,
              network_resume_pb_seam_mark_,
              Tier::SCOPED, false, "network.resume_pb_seam_mark",
              "declare the warm-resume PHASE-BIAS seam on the wire: the first post-resume emit "
              "marks datum_changed for a constellation whose restored per-key baselines moved "
              "past discThresholdM on more than 10 % of the jump test's own population "
              "(measured ~17 m p50 with datum_changed 0 today, SPEC_event_harness s21.9)" },
            // --- RS-5 (SPEC_event_harness s21.10/s21.11): READ half - restore both emit engines
            //     at the seam.  Forward-only resume policy, not fingerprinted; requires
            //     resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_emit_restore_,
              Tier::SCOPED, false, "network.resume_emit_restore",
              "restore the eq34 emit re-base frame + the PB emission filter at a warm resume: emitted "
              "phase biases match the continuous wire to p50 0.000 m at +1, a stateful consumer sees 0 "
              "PB resets at the seam (control 19); products otherwise byte-identical (RS-5)" },
            // --- RS-6a (SPEC_event_harness s21.13/s21.14): READ half - restore both station-layer
            //     memories at the seam.  Forward-only resume policy, not fingerprinted; requires
            //     resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_station_restore_,
              Tier::SCOPED, false, "network.resume_station_restore",
              "restore the rx-CB carry + the T5 deviation carry at a warm resume: a00 transient "
              "+4.6 -> +0.4 TECU, +2 clock contrast 0.44 -> 0.29 m, receiver clocks 1.21 -> 0.91 m; "
              "the code-bias level and the rx-CB gate share do not move (RS-6a)" },
            // --- RS-9 (SPEC_event_harness s21.18/s21.19): READ half - restore the rx-CB frame at
            //     the seam.  Forward-only resume policy, not fingerprinted; requires
            //     resume_station_restore.
            { warm && network_resume_station_restore_,
              network_resume_rxcb_lin_restore_,
              Tier::SCOPED, false, "network.resume_rxcb_lin_restore",
              "restore each station's code-bias linearization point at a warm resume: rx-CB withheld "
              "share 26.3 -> 9.8 % (the seed's 11.0), the 11 defect-created stations back to folding, "
              "receiver clocks at +1 0.93 -> 0.82 m (RS-9)" },
            // --- RS-8 (SPEC_event_harness s21.16/s21.20): READ half - restore the canonical-signal
            //     map before any station's first epoch.  Forward-only resume policy, not
            //     fingerprinted; requires resume_frame_restore.
            { warm && network_resume_frame_restore_,
              network_resume_signal_canon_restore_,
              Tier::SCOPED, false, "network.resume_signal_canon_restore",
              "restore the canonical-signal latch at a warm resume: chord births at +1 64 -> 1, L2X "
              "keys 25.6 -> 0.62 m and every other key 1.50 -> 0.60 m raw, eq34 rerefs 8 -> 2, "
              "clock contrast at +1 0.31 -> 0.16 m (RS-8)" },
            // --- RTA-4 serialize_disc_counters (first ledger row 2026-09-12, RS-4): the per-key
            //     discontinuity counters reach the wire (optional P/B/C fields).  Outputs-only;
            //     changes product bytes, so the goldens pin it only with a recorded re-baseline.
            { true,
              network_serialize_disc_counters_,
              Tier::SCOPED, false, "network.serialize_disc_counters",
              "serialize the per-key phase / code / clock discontinuity counters so a FILE consumer "
              "sees the re-realizations the in-process consumer reacts to (smoke: only P/B/C records "
              "change, b9da8cc1 -> 87a33bf0); pin on the goldens at the RS-5 close-out re-baseline" },
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
            // Card 1f/1g — only meaningful where the Card-1 coupling rows actually EXIST:
            // the conditional carry armed and NOT superseded by the Card-4 invariant carry
            // (under carry_invariant the whole coupling trio is structurally inert, which is
            // this ledger's own "BIGGEST FINDING").  Both rows are fingerprinted.
            // Feed interlock: without eq34 or the prioritized phase-anchor carry re-reference
            // there is no realization-step source at all and the card is inert (measured).
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
     * @brief The COUPLED bundles — sets of knobs that are only correct TOGETHER.
     *
     * Arming a strict subset is its own failure mode (the MW/WL lane stack without
     * @c cold_clock_seed quarantines its lane cycles — measured @c x_lin 269 km at 305
     * stations), so a partially armed engaged bundle WARNs while a fully unarmed one
     * only informs.
     *
     * @param r  Regime facts from @ref networkPostureRegime_.
     * @return The bundle table for this configuration.
     */
    std::vector<GnssConfig::PostureBundle>
    GnssConfig::networkPostureBundles_(const PostureRegime& r) const {
        // Alias preamble: the rows below are the original table text, unedited.
        const auto& nc   = network_config_;
        const bool  lane = r.lane;

        return {
            { lane, "MW/WL lane stack",
              "the lane rows are built in the CORRECTION frame, so an unseeded cold receiver "
              "clock lands in the ambiguity seeds and lane_max_abs_xlin_m quarantines the "
              "cycles (x_lin 269 km measured at 305 sta); the lane also rides the Card-4 "
              "chord basis, and lane_carry additionally REQUIRES ewl_enable",
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
                // The bundle member is "the satellite code bias has a DECLARED
                // source", not "W6 specifically" — EXT-CB satisfies it from the
                // external .BIA.  Naming only W6 here would fire a spurious
                // partially-armed WARN on every EXT-CB run.
                { "network.estimate_sat_code_bias | network.external_sat_code_bias",
                  estimate_network_sat_code_bias_ || network_external_sat_code_bias_ } } },
        };
    }

    /**
     * @brief Log, once per run, every knob the field record recommends for THIS run's regime
     *        and whether it is actually ON — plus the integrity of each COUPLED bundle.
     *
     * Motivation (user, 2026-07-29): the project ships new work default-OFF and
     * byte-identity-gated, which is right for new work but leaves no PROMOTION step, so
     * validated-good options accumulate in the OFF column and are re-discovered by accident
     * one wasted run at a time.  The loader already logs everything it ARMS; this logs the
     * inverse, so a run cannot start without the gap being visible.
     *
     * Two design rules, both paid for with data:
     *  - **Regime, not blanket.** A knob validated at one scale can be harmful at another
     *    (`mw_wl.bridge_sample_debias`: validated at 50 stations, net-harmful at 305 —
     *    short-baseline wrong-integer rate 0.0 % → 12.5 %).  Every entry carries the regime
     *    it was validated in and is only reported when that regime matches this run.  That
     *    filter lives in @ref networkPostureRegime_ and each row's `applies` predicate.
     *  - **Bundles, not knobs.** Several options are only correct TOGETHER; arming a strict
     *    subset is its own failure mode (the MW/WL lane stack without `cold_clock_seed`
     *    quarantines its lane cycles — measured `x_lin` 269 km at 305 stations).  A partially
     *    armed bundle WARNs; the table lives in @ref networkPostureBundles_.
     *
     * Log-only: reads already-parsed state and changes nothing.  Full evidence and the
     * SCOPED/UNVALIDATED tiers live in `docs/CONFIG_promotion_ledger.md`; check any config
     * offline with `scripts/config_posture_check.py`.
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

        // ON first, then SCOPED: the report is ONE pass, so the append order is what keeps
        // the two blocks contiguous in the log (the pre-split table hand-sorted itself and
        // said so in a comment — this makes it structural).
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
        // The table is pre-sorted ON-then-SCOPED, so one pass keeps the two blocks
        // contiguous in the log.  ON-tier gaps WARN — they have no known counter-scope and
        // are exactly what gets re-discovered by accident one wasted run at a time.  SCOPED
        // gaps are INFO: there are 40+ of them, and warning on all would drown the tier that
        // matters.  Both are still reported, and both are still one grep away.
        for (const auto& it : items) {
            if (!it.applies) continue;
            const bool on_tier = it.tier == Tier::ON;
            if (it.on) { (on_tier ? on_armed : sc_armed) += 1; continue; }
            (on_tier ? on_off : sc_off) += 1;
            if (on_tier) log->warn("  POSTURE OFF  [ON]     {} — {}", it.knob, it.why);
            else         log->info("  POSTURE OFF  [SCOPED] {} — {}", it.knob, it.why);
            // CHAIN axis: on a warm resume a fingerprinted recommendation is NOT actionable
            // here.  Say so on the same line group, or the readout becomes an instruction to
            // break the lineage.  Always WARN — this one is a correctness trap, not a tier.
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
