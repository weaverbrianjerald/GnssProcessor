# BOF: the built-but-off register, worked through on the current golden

**STATUS**

- **id:** BOF
- **topic:** config-ledger
- **status:** closed
- **summary:** The owed checks of [OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md), run on the golden of main `ef9f75d` with one exe: a 24 h DOY 129 day (control, PIV, OTL, NTA), a second day for PIV, the block-Baarda positive control, the NSD seed counts, the ARL slip fixes on the field, posture C at 305 stations and a 2 h triage of the 22 never-graded keys. OTL, NTA, BBE (estimable form) and NSD pass their gates; PIV misses one clause (ARHT00ATA on DOY 130); the ARL fixes stay off. ARMED 2026-10-28 at the author's decision (section 6): the estimable block test and the seed mirrors on both goldens, ocean tide loading and the noon-turn attitude on the field golden; the loader warns on the out-of-scope combined sigma; baseline pair `runs/_rf0hv17_{field,sim}6`.
- **source:** `docs/status/OPEN_ITEMS_BUILT_OFF.md` (2026-10-28) and the survey behind it; the cards each item cites
- **knobs / posture:** read, not changed: `network.pivot_election`, `model_corrections.enable_ocean_tide_loading` with `otl_from_sif`, `otl_nodal_corrections`, `otl_remove_diurnal_translation`, `processing.enable_noon_turn_attitude`, `network.block_baarda_estimable`, `network.readmit_seed_mirrors`, `network.cycle_slip.gf_arc_jump`, `network.cycle_slip.if_phase_td_reseed_on_flag`, the four posture-C keys, and the 22 keys of section 3. Section 6 edits the ledger, the manifest and both goldens for the four armed items.
- **dates in the text:** 2026-10-28
- **hashes / run dirs:** exe `build/x64-Release/gnss_processor_bof.exe` (a copy of `gnss_processor_mrf.exe`, the Release build of main `ef9f75d`, sha256[:16] 2d84a326fd00fd04); run root `D:/GnssProcessorSupport/runs_nom`: arms `_rap_bof_*`, census replays `_rap_bofc_*` and `_rap_bofc24_*`, bases `bof_bases/`, gates and grades `bof_reports/`; 6 h control `runs/_rf0hv16_field6` (card MRF's baseline, same exe); arming exe `gnss_processor_bofarm.exe` (sha256[:16] 91143c81de9a9547), baseline pair `runs/_rf0hv17_{field,sim}6`, identity controls `runs/_rap_bofid_{field,sim}6`
- **gates (frozen at pre-registration):** `D:/GnssProcessorSupport/runs_nom/bof_reports/preregistration.md`, written before any arm ran

**Outcome.** Question: which built-but-off items pass their owed checks on the current golden? Ocean tide loading
passes over a whole tidal day (census -1.0 mm, user -0.8 mm, no site worse). Noon-turn attitude passes and moves users
by about 0.1 mm. The estimable block test sees injected code multipath that no other armed check reports, while the
armed whole-block form reports nothing. The coverage pivot election passes on DOY 129 (forced handover exercised) but
misses one site clause on DOY 130 (ARHT00ATA +40.6 mm). The NSD key is product-identical. The ARL fixes cost churn and
stay off. Posture C runs at 305 stations at 2.5 times the cost. Armed at the author's decision: OTL and NTA (field), the estimable block test and the seed mirrors (both goldens); baseline pair `_rf0hv17` (section 6).

## 1. Arms

Every arm is a golden copy with one change, made by `scripts/rapid_arm.py`, run on the one exe above; every control is
the same exe. The census is the 16-site consumer-only replay (`scripts/clock_level/gcensus.py`), graded as each site's
mean hourly p50 over hours 2 to N, paired against the control; a site is worse when it is worse by more than 1 cm.

| item | arms | length |
|---|---|---|
| PIV, OTL, NTA | `bof_ctl_f24`, `bof_piv_f24`, `bof_otl_f24`, `bof_nta_f24` (DOY 129) | 24 h |
| PIV, second day | `bof_ctl130_f6`, `bof_piv130_f6` (the golden moved to DOY 130 by `scripts/mday_golden_config.py`) | 6 h |
| BBE positive control | `bof_bbe_s2_ctl`, `_mp05`, `_mp15`, `_slip` (sim, estimable form on, faults at six stations) | 2 h |
| NSD, ARL | `bof_nsd_f6`, `bof_gfaj_f6`, `bof_rsof_f6` against `runs/_rf0hv16_field6` | 6 h |
| posture C at 305 stations | `bof_ge305_f2`, `bof_c305_f2` (base `bof_bases/golden_field_305.json`) | 2 h |
| 22 never-graded keys | `bof_tri_<key>_f2` against `bof_tri_ctl_f2`; sim `_s2` for the slip keys and the movers | 2 h |

## 2. Results by item

| item | gates | result | recorded in |
|---|---|---|---|
| OTL | O1, O2 pass | WTZZ00DEU -0.84 mm (hours 2 to 24); census -0.98 mm, POHN00FSM -11.4 mm, none worse | [OTL](OTL_ocean_tide_loading_source_2026-10-16.md) section 10 |
| NTA | N1 to N4 pass | census -0.002 mm with and without the model at the consumer; WTZZ00DEU -0.29 mm, -3 to -4 mm in the two hours after G25's turn | [NTA](NTA_noon_turn_attitude_2026-10-16.md) section 9 |
| BBE | B1, B3 pass; B2 = 0 | 1.5 m code multipath: 218 flags against 9 at the same stations; no other armed tier reports it; the armed whole-block form reports nothing | [BBE](BBE_block_baarda_estimable_2026-10-18.md) section 9 |
| NSD | S1, S2 pass | products identical; 23 code-bias seeds in 6 h now taken by the monitor and the census | [NSD](NSD_network_small_defects_2026-10-17.md) section 8 |
| PIV | P1 to P4 pass on DOY 129; P5 fails | forced handover exercised; handovers 1 against 2; census -0.13 mm; DOY 130 ARHT00ATA +40.6 mm; GPS clock p50 +4.8 mm | [PIV](PIV_pivot_election_coverage_2026-10-17.md) section 9 |
| ARL slip fixes | A2, A3 pass | users within 0.2 mm; the arc-jump flag triples the slip re-levels (1011 to 3100) and the anchor-slip handovers go 5 to 57 | [ARL](ARL_integer_ambiguity_ladder_2026-10-09.md) section 14 |
| posture C, 305 stations | reported | runs to the end, withholds nothing, 38 BeiDou clocks; WTZZ00DEU hour 1 -19.7 mm, hour 2 +7.9 mm; loop 2.5 times GE | [MDAY](MDAY_multiday_candidate_2026-10-19.md) section 10 |

## 3. The 22 never-graded keys, 2 h field triage

Worker report `D:/GnssProcessorSupport/runs_nom/bof_reports/triage_2h.md` (values, companions, commands); the consumer
numbers below recomputed from `logs/closed_loop_trace.csv`. Control WTZZ00DEU p50 hours 1 and 2: 57.00 and 11.65 mm.
Every run exits 0 and withholds nothing.

| reading | keys (value armed) |
|---|---|
| identical | `adm_revet`, `raim_withhold_adapt_escalate` (12), `raim_withhold_free_erase`, `per_const_withhold`, `gross_los_station_tier`, `signal_min_station_fraction` (0.3), `phase_bias_min_station_fraction` (0.3), `cycle_slip.if_phase_td_ref_elev_norm`, `ura_retro_bump`, `require_signal_integer_for_ar`, `pivot_vanish_return_signal`, `ura_refloat_inflate` (2.0) |
| diagnostics or the product file only, consumer identical | `clock_monitor` (0 flags), `cycle_slip.if_phase_td_ref_warmup_abstain` (reference fires 4 to 3), `contract_rebroadcast_s` (30: S lines 1 to 123), `emitter_staleness_horizon_s` (900: header only), `mark_refsat_reset_disc` (one R record), `warmup_ura_floor_m` (1.0) |
| the solve moves | `signal_integrity.census` (T4 refloats 60 to 30; consumer hour 2 -0.40 mm), `raim_refloat_maturity_gate` with `raim_refloat_maturity_epochs` 120 (refloats 60 to 8; hour 2 -0.45 mm) |
| the consumer breaks | `closed_loop.consume_combined_sigma` with `emit_combined_sigma`: 1470 / 409 mm (sim 604 / 311 mm against 99 / 33); `combined_sigma_agek` (0.001) needs the same pair and reads the same |

Seven of the identical keys cannot act on a cold golden at all (no withheld epoch, no pivot change, the consumer's AR
off, no flagged or refloated satellite): identical means not exercised, not harmless. Six values had no graded source
and were chosen for the triage (maturity 120 epochs, the two fractions, 900 s, agek 0.001, inflate 2.0, floor 1.0 m).
The sim has no IF-TD candidate and cannot exercise the two slip keys.

## 4. Findings beyond the gates

- **Code multipath of 1.5 m is reported by no armed check.** At the six injected sim stations RAIM and the census
  cross-flag emit nothing, MDB reads 44 events in every arm and the prior-residual census moves 241 to 261. Only the
  estimable block test sees it; the armed whole-block form refuses all 16815 blocks of the sim golden in 6 h. A
  response (beyond a report) to code multipath does not exist on either form.
- **The combined-sigma consumer key is a trap.** The ledger scopes `closed_loop.consume_combined_sigma` to consumers
  that take only the biases; the goldens' consumer takes the network clock. The loader accepts the combination without
  a word and the consumer's error grows by metres. A loader refusal or warning for the out-of-scope pairing would
  close it.
- **ARHT00ATA decides every "no site worse" clause.** It is the census site with the largest error and moves by
  centimetres under unrelated wire changes (-16.8 mm under the arc-jump flag, +40.6 mm under the pivot election on
  DOY 130, +59 mm under the sum-to-zero competitor of card PIV). Its spread under a neutral perturbation is not measured.
- **Over a whole day OTL reverses its 6 h reading.** The held-out consumer went from +2.0 mm (00:00 to 06:00) to
  -0.8 mm (hours 2 to 24) with a diurnal pattern in the hourly change: a 6 h window grades a tidal term partly.

## 5. Decisions owed to the author

| item | evidence | recommendation |
|---|---|---|
| OTL, the four keys | gates pass over a whole tidal day | arm |
| `network.block_baarda_estimable` | the only check that reports code multipath; measure-only; 0.7 to 0.9 times the wall of the blind form | arm (keeps `block_baarda`) |
| `network.readmit_seed_mirrors` | products identical; closes a 23-seed telemetry gap | arm |
| `processing.enable_noon_turn_attitude` | gates pass; about 0.1 mm at users; inert on the goldens' window | arm or leave: a model correction with no measurable user cost or gain |
| BeiDou, posture C | five days better at 58 stations; at 305 stations hour 2 +7.9 mm on one 2 h run; cost 2 to 2.5 times | the author's; a 6 h 305-station pair would settle the hour-2 flag |
| `network.pivot_election: coverage` | DOY 129 passes, DOY 130 fails on ARHT00ATA alone; GPS clock p50 +4.8 mm | hold until the ARHT00ATA spread is measured |
| ARL fixes 1 and 2 | consumer-neutral, churn up to three times | keep off |
| `signal_integrity.census`, `raim_refloat_maturity_gate` | fewer refloats, consumer -0.4 mm on one 2 h arm | a 6 h arm and census before any decision |
| the consume-combined-sigma trap | metres at the consumer when armed out of scope | a loader guard (a code change) |

Unchanged by this card and not run: the satellite clock-step fixes (branch `claude/sck-sat-clock-step`, blocked on card
CRS), the research tracks (IRD-1, ALD, AB3, SNXB phase 2), `network.streaming.enable` and
`realtime.tcp_silent_timeout_s` (need a live source), the seams and warm resume (a scope-cut add-back; the goldens do
not resume, which also makes the posture-C warm resume field-inert), and the earlier decisions on WGB, N3c D7 and D1 and
NIS (their evidence is unchanged).

## 6. The author's decisions and the arming, 2026-10-28

Armed at the author's decision, through the promotion path:

- **Both goldens:** `network.block_baarda_estimable` (ledger UNVALIDATED to TRACE, golden, beside `block_baarda`) and
  `network.readmit_seed_mirrors` (UNVALIDATED to RECOMMEND_SCOPED, regime `readmit_seed_horizon_s > 0`).
- **Field golden only:** `model_corrections.enable_ocean_tide_loading` with `otl_from_sif`, `otl_nodal_corrections` and
  `otl_remove_diurnal_translation` (the three rows RECOMMEND_SCOPED, regime ocean tide loading on), and
  `processing.enable_noon_turn_attitude` (RECOMMEND_SCOPED, regime GPS or GPS + Galileo). The sim fixture carries neither
  loading nor a noon-turn law: the sim golden leaves ocean tide loading off (so the three OTL rows are out of regime
  there) and lists the noon-turn key as a documented exception.
- **Loader:** four SCOPED posture rows (`GnssConfigNetworkPosture.cpp`); `config_posture_check.py --verify-loader` 0
  coverage gaps; the loader defaults stay off. `make_golden_config.py` regenerated both goldens (field 82 recommended
  knobs armed in regime and 2 documented exceptions, sim 78 and 3; verify PASS); the goldens differ from main only in
  the seven field keys and the two sim keys.
- **Loader warning (the combined-sigma trap of section 4):** `network.closed_loop.consume_combined_sigma` with
  `consume_network_clock` now logs a warning at load (`GnssConfigLoadNetwork.cpp`, suite `CombinedSigmaScopeTest`, 3
  cases). A refusal would break `config/test_suite/_seamctl_replay_warm_csig.json`, a seam control that pairs the two
  on purpose.
- **Tests:** `StashStatusSeedMirrorTest.TheKeyDefaultsOffAndNeitherGoldenArmsIt` pinned the unarmed goldens and became
  `...BothGoldensArmIt`. Debug suite 3613 passed, 1 fixture-gated skip, 0 failed.

**Baseline pair** `runs/_rf0hv17_{field,sim}6`, exe `build/x64-Release/gnss_processor_bofarm.exe` (the Release build of
this tree, sha256[:16] 91143c81de9a9547). Gates, written before the runs:

| gate | compared | result |
|---|---|---|
| G1 | new field golden on the new exe against the old golden plus the seven keys on the old exe (`runs/_rap_bofid_field6`) | PASS: products, closed-loop trace, solution, epoch status and block census identical; only wall-time columns differ |
| G2 | the same for the sim and its two keys (`runs/_rap_bofid_sim6`) | PASS, same files |
| G3 | new sim golden against `runs/_rf0hv16_sim6` | PASS: products, solution and closed-loop trace identical; the block census, carry VCE, prior-residual census, events and epoch status differ (the two keys are measure-only and telemetry) |

Read-outs, held-out consumer 3D p50 by hour, epochs below a decimetre:

| run | hours 1 to 6 | below 0.1 m |
|---|---|---:|
| field `_rf0hv17_field6` (WTZZ00DEU) | 0.0588 0.0123 0.0231 0.0123 0.0165 0.0120 m | 688 / 720 |
| field `_rf0hv16_field6` | 0.0570 0.0117 0.0183 0.0103 0.0155 0.0105 m | 688 / 720 |
| sim `_rf0hv17_sim6` (SZZ000SIM, from the run start) | 0.1368 0.0723 0.0307 0.0239 0.0257 0.0182 m | 595 / 720 |

The field golden window (00:00 to 06:00 UT) reads WTZZ00DEU 2.0 mm worse over hours 2 to 6 than the pair before it,
the number card OTL's 6 h arm gave (section 9.5); over the whole tidal day the same keys gain 0.8 mm at WTZZ00DEU and
1.0 mm at the census median (section 2). A 6 h golden grades a tidal term over a quarter of its cycle, so the golden's
WTZZ00DEU baseline moved for a reason that is not a regression. The field block census tests 60209 of 60302 blocks
(93 refused) and reports 4155 (6.9 %), none block-only; 7 roster stations run with zero loading (no SIF block).
