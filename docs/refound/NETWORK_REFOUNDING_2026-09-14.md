# Network re-founding — the author's decision of 2026-09-14

**Status:** OPEN — the program that replaces the card-by-card patching of the network producer.  Everything under `docs/refound/` is
graded on the two instruments named below and nothing else.  Entry point for the network from this date on.

## The decision (author, verbatim)

> Yes, I agree that our immediate next steps are:
> Freeze the posture at your model on the field: leveling vehicle + gauge projection + phase-datum family, free slant iono (drop SH from
> the producer — it cost ~0.3 m on the wire in b5, not the defect either way), physical clock time-link, the clock step test.  Consumer
> ~1–2 m today.
> Replace the mechanism zoo where the evidence says it is broken, in this order: ambiguities as float states with covariance (no
> re-seeding levels from one code epoch); a fixed S-basis with declared S-transformations at anchor handovers (minimum age, elevation —
> the pivot S-transform already exists); QC that acts on code rows and a wire contract that declares every step.
> Grade only on the cold 6 h consumer and the wire-vs-CODE census.  Nothing else.
> Cut scope until that consumer is sub-decimetre: GPS-only, dual-frequency, no lanes, no F3, no seams.  Add one thing back at a time.

And the workflow rule the author named: **sim first, then field** — which the project followed per card but not for the vehicle: the field
golden's carry (`carry_invariant`, July) diverged from the sim golden's (leveling) and was never re-validated as a whole.  From this date:
**one posture, two beds** — the sim golden and the field golden share the vehicle and the knob set; every card is validated on the sim
truth bed first, then on the field; the field golden never diverges from the sim's vehicle again.

## Why (the evidence, one line each; the record is SPEC_event_harness §24.9–§24.20)

- The field golden's station block is SINGULAR (null dim 7 at 58/61 stations, all hour): the rank deficiency was solved for the shared
  system's datum, never for the local block — the ZP trace (§24.10) is the first instrument that looked.
- The sim golden's vehicle on the field (PV-B, §24.11): block full-rank in ten epochs, consumer 7.8 → 1.2 m.
- The free satellite clock random-walked at 0.17 m per step; the Q3 anchor at the physical floor (§24.16): code rms 1.79 → 1.09 m,
  consumer 0.95 m.
- Gross code errors were unreachable by every acting screen (§24.18); 43 satellite clock steps to 16.7 m reached the wire unsignalled
  because the existing step test was never armed (§24.19 R10; §24.20 fixes it).
- The wire's within-arc random walk is the refloat re-realizations of the satellite clock (88 % at a T4 refloat; 42 % on the anchor
  station) — the zero-q "level" managed by responses, not a float state (§24.19 R7 / R9 / R10).

## The frozen posture (RF-0 builds and certifies it)

**Field** = `config/golden/golden_field_gps_dual.json` +
- the leveling vehicle (SPEC_event_harness §24.11's 16 keys: `carry_invariant` and its sub-family off, the EWL/UWL lanes off,
  `cycle_slip.relevel_free_epoch`, `carry_coupling_gauge_project`, `phase_datum_eq34: false`, `phase_datum_prioritized`,
  `phase_datum_handover_continuous`, `phase_anchor_carry_reref`, `phase_datum_min_stations: 2`);
- the physical clock time-link (§24.16's 12 Q3 keys with `clock_anchor_q: 2.9e-8`);
- the wire contract: `network.clock_disc_jump_test: true` (§24.20);
- free slant iono: `network.estimate_iono_sh: false` (the loader's dependent forcings recorded; the SH field returns later as a CONSUMER
  product, never as a producer constraint);
- the scope cut: GPS only, dual frequency (L1/L2 — the config that makes dual solvable under W6 is RF-0's first finding: §24.14 c5 showed
  `frequency_mode: dual` alone leaves W6's L5 code-bias columns registered but unobserved), lanes off, F3 off, seams / snapshots / warm
  resumes off, the WTZZ consumer cold.
**Sim** = `config/golden/golden_sim_tri_w6.json` restricted to GPS dual + `clock_anchor_q: 2.9e-8` + `clock_disc_jump_test: true`, the same
scope cut.  The two configs live in `config/refound/` and differ ONLY in the bed (stations, data, truth) and the bed-specific datum
station.

## The two instruments (the only bars)

1. **The cold consumer, 6 h**: per-hour p50 and p90 3D (field: WTZZ00DEU against ITRF; sim: the bed's truth consumer), and the epoch of
   first sub-decimetre.  Target: sub-decimetre settled.
2. **The wire against a reference, per hour**: field — `scripts/wire_vs_code_census.py` (within-arc D_phase / D_code, the clock LEVEL);
   sim — the same against truth.
Everything else (code residuals, ZWD, σ̂₀, DD fractions, level channels) is DIAGNOSTIC — read to explain a delta, never a bar.

## The cards, in order (each: sim first, then field; graded as a delta against RF-0 on the two instruments; default OFF until it wins)

| card | goal | replaces |
|---|---|---|
| **RF-0** | the frozen posture on both beds, 6 h cold, the baseline numbers | — |
| **RF-1** | ambiguities as float STATES with covariance in the local block: an uninformative prior at birth converging from code + phase; a slip = drop the prior (refloat), never a re-seed from one code epoch; the carried covariance folded, not a zero-q value | the leveling inject, `relevel_free_epoch`, the F3 block, the D5 gates, the `carry_invariant` family |
| **RF-2** | a fixed S-basis with DECLARED S-transformations at every anchor / pivot handover (the pivot S-transform exists — `handover_stransform`); the anchor elected by elevation and a minimum age, never a newborn low arc; no incumbent refloat without a declared handover | the coverage-only election, the cold PB-gauge re-realization |
| **RF-3** | QC that acts on CODE rows (the gross-LOS screen made consumer-safe by the satellite stand-down landing BEFORE emission) and the wire contract on every channel (clock, PB, CB steps all declared) | MDB observe-only, the phase-only pools |
| add-backs, one at a time | the SH field as a consumer product; L5; lanes; seams / warm resume | — |

## Status

| date | item | result |
|---|---|---|
| 2026-09-14 | decision recorded; RF-0 and the RF-1 design read launched | — |
| 2026-09-14 | RF-1 design read landed — [RF-1_float_ambiguities_design.md](RF-1_float_ambiguities_design.md) | the ambiguity is a managed level, not a state; the covariance fold exists (F3); the anchor handover erases every station's carry |
| 2026-09-14 | RF-1 decisions — [RF-1_decision_2026-09-14.md](RF-1_decision_2026-09-14.md) | build GO in the design's commit order; steps 1–2 (knob + measure-only trace) launched, their D1–D4 read gates the mechanism |
| 2026-09-14 | RF-0 baseline landed — [RF-0_baseline_2026-09-14.md](RF-0_baseline_2026-09-14.md) | field consumer 1.0–3.3 m with no trend, sim 0.57 m at hour 6 and converging; dual needs `rx_code_bias_datum`; SH-off falls to the external GIM (RF-0b pre-registered) |
| 2026-09-15 | RF-0b closed — [RF-0b_dcb_datum_2026-09-15.md](RF-0b_dcb_datum_2026-09-15.md) | W6 runs without the GIM, but the 31 per-satellite geometry-free directions were being closed by the GIM and are now closed only by the Kaula prior; NOT the baseline; RF-0c (a hard per-satellite GF datum) pre-registered |
| 2026-09-15 | RF-1 steps 1–2 closed — [RF-1_steps12_read_2026-09-15.md](RF-1_steps12_read_2026-09-15.md) | the field carry's variance collapses 54 700× over 30 folds (b 3.44) where the joint-block bed reads 1/n (b 1.02); the mature prior is 100× over-confident; the mechanism (steps 3–5) is warranted and launched |
| 2026-09-15 | RF-0c closed — [RF-0c_gf_pin_2026-09-15.md](RF-0c_gf_pin_2026-09-15.md) | the hard per-satellite GF pin is a real datum (31 → 0 directions, prior off); `broadcast_tgd` is the posture (beats zero by 5–13 m on the field consumer every hour); not yet the baseline — the sparse L2X channel needs its own per-signal pin and the thin-satellite phase gauge remains (RF-0d pre-registered) |
| 2026-09-15 | RF-1 steps 3–5 closed — [RF-1_steps345_mechanism_2026-09-15.md](RF-1_steps345_mechanism_2026-09-15.md) | the float vehicle is CORRECT (1/n on both beds; sim DD error 14× smaller, 99 % WL integers, consumer 0.57 → 0.30 m) and not sufficient on the field: the phase datum is now a live state and churns; RF-2 sharpened and launched (held S-basis, every handover declared, election by elevation and age) |
| 2026-09-15 | RF-0d closed — [RF-0d_per_signal_pin_2026-09-15.md](RF-0d_per_signal_pin_2026-09-15.md) | the per-signal pin is a datum (L2X 31 → 0 directions; ignition withholds 29 → 5) and closed RF-0c's inconsistent sum-to-zero rows; the field consumer regressed 4–15 m because the constellation LEVEL anchor went with them; the OFF smoke header hash is now `47bc4e5a…`; RF-0e (one IF-level row per constellation + the L2X coverage cut) pre-registered |
| 2026-09-15 | RF-2 open — [RF-2_held_basis_2026-09-15.md](RF-2_held_basis_2026-09-15.md) | the held S-basis collapses the field's within-arc phase wire below RF-0 in every hour (erases 0, drift 0); the sim fails on the election composing wrongly with the ledger screen (67 % cold elections, integers lost); the field consumer tracks a clock-level ramp; RF-2b (the one-predicate fix + the clock-ramp read) authorized |
| 2026-09-15 | RF-0e closed — [RF-0e_level_datum_2026-09-15.md](RF-0e_level_datum_2026-09-15.md) | the level datum is not the term (the consumer tracks the per-satellite IF-correction dispersion); the sparse C2X channel is: cutting it gives 0.82 m at hour 2 — the best field consumer on record — then starves two C2X-only stations; RF-0f (C2X in the solve, its ISC a per-satellite state, out of the pin) pre-registered |
| 2026-09-15 | RF-0f closed — [RF-0f_isc_state_2026-09-15.md](RF-0f_isc_state_2026-09-15.md) | the ISC-as-state mechanism is right and unusable here: the C2W and C2X station sets are disjoint (25 vs 2, none in common), so C2X leaves the wire and the solve is e2; the OFF smoke header hash is now `9add1d3a…`; RF-0g (the band-2 cut + the two C2X-only stations off the roster) pre-registered |
| 2026-09-15 | RF-2b closed — [RF-2b_election_fix_2026-09-15.md](RF-2b_election_fix_2026-09-15.md) | the sim fully restored (720/720, 99.1 % WL integers); the field keeps the phase-wire win at 47 handovers/h, the clock ramp gone, the consumer 0.77–1.48 m every hour — RF-1 + RF-2b is the vehicle candidate; R-CL: the clock level runs through handovers on every arm — the clock's own level channel (0.69 m / 100 s) is the next term; RF-2c (the attribution read + the gauge-free arm) pre-registered |
| 2026-09-15 | RF-0g closed — [RF-0g_roster_cut_2026-09-15.md](RF-0g_roster_cut_2026-09-15.md) | the best field posture in the record (3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 m, no collapse; IF dispersion 0.51 m PASS) and not the baseline: an ignition deficit withholds 75 epochs in hour 1 and hours 5–6 drift; next (not launched): RF-1 + RF-2b on this posture, and RF-2c's clock-level attribution |
| 2026-09-15 | correction to the RF-2b row — [RF-3-prep_golden_rebase_2026-09-15.md](RF-3-prep_golden_rebase_2026-09-15.md) §1 | RF-2b's consumer figures were ratios to RF-0, not metres: its field consumer was 2.32 / 2.83 / 2.29 / 1.89 / 1.23 / 3.05 m (about RF-0's) and its sim 1.54 / 2.53 / 3.90 / 4.69 / 6.61 / 1.54 m; "0.77–1.48 m every hour" is withdrawn |
| 2026-09-15 | RF-3-prep closed, the goldens re-based — [RF-3-prep_golden_rebase_2026-09-15.md](RF-3-prep_golden_rebase_2026-09-15.md) | RF-1 + RF-2b on RF-0g fails both beds (field 564 / 720, IF-correction dispersion p90 0.51 → 3.03 m; sim consumer to 12.98 m) — KEEP OFF; on the corrected record RF-0g is the best posture on both beds and the only self-contained one, so the goldens are now `config/golden/golden_field_refound.json` / `golden_sim_refound.json` (verify PASS; each certified by a 6 h run that reproduces RF-0g byte-for-byte); the two previous goldens retired, frozen at their paths; RF-3-read, the ignition deficit and RF-2c pre-registered on the golden posture |
| 2026-09-15 | RF-3-read + RF-2d closed — [RF-3-read_handover_frame_2026-09-15.md](RF-3-read_handover_frame_2026-09-15.md) | the vehicle's IF-correction dispersion is 100 % satellite clock (no GF-bias part) and steps at COLD held-basis handovers: two RF-2b bookkeeping defects — the declaration predicate inverted against the frame (144/145 continuous marked, 10/12 cold unmarked) and the cold families' retired-frame release cleared; RF-2d (`phase_datum_held_cold_release` + `phase_datum_held_mark_cold`, default OFF, OFF smoke identical) fixes both: SIM consumer 3.14 / 0.47 / 0.24 / 0.24 / 0.28 / 0.33 m (golden 6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55), IF dispersion p90 0.043 m; FIELD settled hours 3–6 0.84 / 0.65 / 1.37 / 0.60 m (golden 1.28 / 1.25 / 1.78 / 2.68) but 573 / 720 emitted (the S-basis null-space gate) and hour 2 3.00 m — not yet in contention; the null-space read pre-registered |
| 2026-09-15 | RF-NS closed, the null-space read — [RF-NS_null_space_read_2026-09-15.md](RF-NS_null_space_read_2026-09-15.md) | the frozen reading's mechanism is FALSIFIED for the mid-run stretches: every one of the vehicle's 67 post-hour-1 withholds AND the golden's own 5 is ONE satellite's `C1W` CODE-BIAS column carrying exactly zero information (weight 1.00, ev 0 to 1e-16) for the whole of that satellite's readmission gap — withheld epochs = `gap_ep - 1` in all three instances (G04 6/5 both runs, G07 50/49, G30 19/18), closing on the `readmit_seed_code_bias` epoch; the column is not in the IF or GF pin pair and leaves its sum-to-zero row when the time-linked veteran set drops it.  The IGNITION deficit IS the phase-datum class: 14-21 per-satellite {CLK, PB x3} lumps the S-basis completion cannot separate because it has no carried level (every `target=0.0000`), dying in ONE epoch at TOW 261660 when the leveling harvest's first arcs appear.  Both halves are a DECLARATION fix, no rank read owed; the fix is pre-registered, NOT built.  The census latch is now re-armable (`network.trace_estimability_budget` / `trace_estimability_deficit_only`, defaults trace-identical; the re-run is byte-identical on products, closed-loop trace and epoch status); fix (a) REJECTED at the re-coupling gate, see §6 — RF-NS-b read |
| 2026-09-15 | RF-NS-b open, the vehicle observation-loss read — [RF-NS-b_vehicle_obs_loss_2026-09-15.md](RF-NS-b_vehicle_obs_loss_2026-09-15.md) | nothing was absent: G07 / G30 / G04 are TRACKED by 10–13 stations on every withheld epoch and `network_if_slip.csv` is sha256-identical between the two runs, so the observation stream is bit-identical.  What differs is how many stations reach the SOLVE: `NetworkNormalEquations::addStation`'s relative PD test on `N_rr_r` (`NetworkNormalEquations.cpp:476-484`) drops a median of **29 of 59 stations per settled epoch in the GOLDEN and 42 in the vehicle** (23 633 / 30 340 drops over 720 epochs; six stations rejected on all 720), so G07 reaches the border at 6–7 stations in the golden and at ONE in the vehicle, and the registered `CB|G07|C1W` column is fed by ≤ 1 in-solve station — registration counts ≥ 2 PRESENT stations (`ApplicationNetworkAssembly.cpp:2108-2117`) before the drops.  The park predicate is IDENTICAL in both arms; the withhold starts on the column's RETURN epoch (the golden's own `col_added` + `seed_code_bias`), and the seed that would pin it is DOWNSTREAM of the gate (`NetworkSolveGlue.cpp:1159` vs `:1048`), so the withhold blocks its own remedy.  G04's 5-epoch dip is the same defect at ignition (golden 2 in-solve of 10 tracked, 38–39 stations dropped), not a genuine absence.  (b1) the PD rejection at its source and (b2) fix (a) v2 with the pinned column quarantined from stash / veteran / park-retire are pre-registered, NOTHING BUILT.  Instrument: `scripts/rf_obs_loss_read.py` |
| 2026-09-15 | RF-PD open, the station-rejection null named — [RF-PD_station_rejection_2026-09-15.md](RF-PD_station_rejection_2026-09-15.md) | `NetworkNormalEquations::addStation`'s PD rejection now NAMES its null direction (`network.trace_station_pd_null`, default OFF, solve byte-identical on both arms and on the OFF smoke).  All 23 633 golden and 30 340 vehicle rejections carry **exactly one** near-zero eigenvalue (`n_eig = 1` on 100 % of rows) with the SAME mass split — AMB 77.8 % / IONO 21.0 % / CBIAS 1.2 % / CLK 0.0 % — whose ambiguity share has p50 0.779 and max 0.781 across 54 k rejections: it is the receiver-DCB confound in closed form (`b = a`, `c = γa`, predicted `(b²+c²)/a² = 1 + γ² = 3.713` vs measured **3.70**; the clock drops out exactly, hence CLK 0.0 %).  The datum that closes it EXISTS and is installed one line too late — the RXCB-DATUM row (`ApplicationNetworkAssembly.cpp:4072-4078`, λ = 0.01, four orders above the 1e-12 bar) goes in at `est.addStationPrior(...)` `:4605`, AFTER `est.addStation(...)` `:4555` runs the test.  What saves the survivors is an accident: a prioritized phase-datum ANCHOR pin (`foldAmb`, 1e10, `:2819-2860`) landing in that station's own block — of 14 370 anchored station-epochs **0** were rejected, and all 23 633 rejections are anchorless station-epochs.  Step 0's signal hypothesis is REFUTED (lacking C1W is sufficient, not necessary; 13 C1W-carrying stations are also always-rejected).  Re-graded counts: 24 golden / 36 vehicle stations rejected on all 720 (not six — that was a `most_common()` prefix); settled medians 29 / 42 of 59; golden hourly 46/39/33/29/26/25 as the anchor roster fills, vehicle 48/44/43/41/40/40 because its phase datum churns.  (a) install the station's own datum rows BEFORE the gate (`network.station_prior_before_pd`), (b) the AMB/IONO half has nothing of its own to fix, (c) declared reduced rank as the backstop — pre-registered, NOTHING BUILT.  Instrument: `scripts/rf_pd_null_read.py` |
