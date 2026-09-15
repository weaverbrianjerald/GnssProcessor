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
