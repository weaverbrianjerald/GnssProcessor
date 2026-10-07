# CE: the chain-era bisect on the leveling vehicle

**STATUS**

- **id:** CE
- **topic:** network-vehicle
- **status:** closed
- **summary:** Bisect run, no named bar crossed: the 17 CHAIN-ERA keys reproduce PV-A byte-identically, the class does not decompose, and only c3 (the a priori clock pin) moves the code side.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `processing.frequency_mode`, `estimation.ionosphere_weighting.elevation_cutoff_deg`, `processing.elevation_mask_deg`
- **hashes named in the text** (commit / exe / byte-cert): `39bcde8`
- **run dirs named in the text:** `runs/_cm_pvb`
- **source:** `docs/archive/SPEC_event_harness.md` §24.14 (its arms' results are in §24.15, now the [LV-SEED](../ionosphere/LV-SEED-per-los-level.md) card)

**Outcome.** The card bisects the 17 CHAIN-ERA keys that separate PV-A from PV-B, whose per-station code rms is 1.79 m; its body holds the §24.13 reads (H1 to H3 REFUTED). Every arm was tried the same day and graded in [LV-SEED](../ionosphere/LV-SEED-per-los-level.md) (§24.14 RESULTS): c0 equals PV-A at 1.19 m on the last-hour read; c1, c5 and c6 cannot run alone; c3 alone reaches 1.08 m; tie cutoffs of 15° and 20° regress, so 25° stays. [RF-0](../../refound/RF-0_baseline_2026-09-14.md) refuted c5's attribution, and the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) posture carries c3's self-contained counterpart, the Q3 clock anchor.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.14 Card CE — the CHAIN-ERA bisect on the leveling vehicle (pre-registered before any arm is read)

Base `_pv_b.json`, control `runs/_cm_pvb`, exe = main's Release after the ZP merge (the fallback makes every arm's code side readable);
2 h cold; one CHAIN-ERA group per arm, values as the flattened pl2 chain has them: **c1** CODE OSB, correctly specified
(`external_sat_code_bias: true`, `estimate_sat_code_bias: false`, `model_corrections.enable_code_phase_bias: true`,
`require_bia_for_signals: true`; the loader's effective posture recorded — if W6 is still forced, the arm is reported inert and the forcing
line is the finding); **c2** `rx_code_bias_datum: true`; **c3** `apriori_clock_anchor_sigma_m: 0.02`; **c4** the SH representation
(`max_degree` 15 + layer 2 + `mean_vtec_prior` 15 ± 30, as pl2); **c5** `processing.frequency_mode: triple`; **c6** the remainder
(`emit_pb_filter_meas_sigma_m` 0.10, `mark_refsat_reset_disc`, `require_atx_coverage`; `cb_kaula_prior` off is already exonerated as b3);
**c0** the UNION of all 17 CHAIN-ERA keys on the golden base (PV-A's posture on the golden's pins); and **pva2** = PV-A itself re-run 2 h
on the fixed exe (its (i) / (iv) now readable on the same last-hour window).  Reads (i)–(v) + the consumer as §24.11.  **Bars (frozen,
absolute = §24.8's)** and the same contributor / exonerated rules against PV-B.  **Frozen reading:** c0 reproduces pva2's code rms (within
0.2 m) ⇒ the class carries the code defect and the single arms name it (an arm halving (i) is the candidate; one that also halves (iii)
or the consumer is the card); c0 does not ⇒ the difference is in GOLDEN-NEWER (bisected next, one pin per arm); c1 crosses (i) alone ⇒
W6's self-estimated satellite code biases are the second code-side defect on this bed (a posture decision: OSB from the external .BIA
against a self-contained producer); any arm that halves (i) but not (iii) confirms §24.13's premise that the wire's within-arc wander is
born on the phase side (H1), which card LV-SEED reads in parallel.

**§24.13 RESULTS (2026-09-14 ~02:30 local; worker on worktree-agent-a91a639c54258a183 `39bcde8` — `scripts/lvseed_read.py`, merged; every
trace already on the three arms, no producer run; manager grade).**  All three hypotheses fail their frozen bars, and every raw statistic
that appears to pass is carried by ONE line of sight (below).
- **H1 REFUTED** (R1, 1656 code arcs per arm): the fraction of arcs whose first-3-epoch level is within 0.3 m of the settled level is
  **0.39–0.46** on every arm, band and birth class (bar > 0.7); the nominal r(first epoch, settled) 0.978 is the DJIG/G06 outlier — Spearman
  0.17, trimmed (|level| < 5 m, 1646 of 1656 arcs) **0.150**; a random mid-arc epoch predicts the settled level BETTER than the first
  (0.993 vs 0.978); 74–77 % of arcs have a level that GROWS over the arc; arcs present at epoch 1 give r = 0.02; per-station code rms vs
  arcs born there ρ = 0.004.  The unrequired R4: wire steps are not associated with births (near-birth fraction 0.000 vs null 0.066).
- **H2 REFUTED** (R2, settled hour): the pre-registered C2W / C1C level ratio is **1.07–1.15** (slope 1.153, trimmed 1.074; γ = 1.647;
  18–22 % of LOS in the γ band) — the per-LOS LEVEL is non-dispersive on this vehicle too (§24.12's "implied dispersive 88 %" is the
  epoch-to-epoch variation, a different quantity); the level against the tie residual (rebuilt as the tie row asserts it, both shells,
  `network_vtec_sh.csv`): r = **−0.361**, ρ −0.400, trimmed −0.361 (bar 0.5; the sign an iono misfit predicts, the size not); b7 − PV-B
  per-LOS Δlevel median |Δ| 0.16 m, flat over elevation and latitude — a 4× looser tie barely moves the level.
- **H3 REFUTED** (R3): a position-vector fit gives |d| > 1 m at 85 % of the 13 ZWD stations AND at 66 % of the other 47 (chance level
  for 3 parameters on 7–12 points; explained SS adjusted for the null 0.25 vs 0.02); the fitted vectors have no relation to the RINEX
  `ANTENNA: DELTA H` (r = −0.03; only 6 of 60 headers exceed 0.3 m, max GRAZ 1.96 m; BOGT 0.061, GOPE 0.111, GAMB 0.000).
**Two findings the reads surfaced instead:**
1. **DJIG00DJI / G06 is one LOS and the entire outlier population** — born at k = 210 at 16.6°, its slant-iono state 81.4 m against the
   SH field's 5.5 m, code residuals −75.5 / −123.9 / −135.4 m on C1W / C2W / C5Q (ratios 1.641 / 1.793 = exactly −γ_b × a 76 m iono
   excess) for its whole life; `network_events.csv`: at its birth epoch (tow 265470) the G06 phase anchor re-anchored PALM00ATA →
   DJIG00DJI with a COLD PB-gauge handover — the satellite's phase datum moved onto a newborn 16° arc carrying a 76 m gross error, with
   DJIG's iono deweight already released (259950) and no RAIM rejection.  Invariant #7's re-coupling edge without re-validation.
2. **The SH tie's admission cutoff is 25° against a 15° observation mask** (`estimation.ionosphere_weighting.elevation_cutoff_deg` 25
   vs `processing.elevation_mask_deg` 15; `ApplicationNetworkAssembly.cpp:1044`; a deliberate choice of the global-SH card): **25.8 % of
   settled iono rows carry no tie row**, and that population owns the whole metre-plus tail — per-LOS level p99 6.34 m / max 138 m below
   the cutoff against p99 3.29 / max 6.1 m above; |tie residual| p99 19.1 / max 85 m against 12.5 / 26.4 m — while the medians are the same
   (0.53 vs 0.46 m).  An admission edge with no counter; it explains the tail, not the bulk.  Same on all three arms.
**Reading:** §24.13's fourth — no hypothesis named; the 0.4–0.5 m per-LOS bulk on PV-B behaves as a noise floor AT THIS POSTURE — with the
caveat §24.11's closure supplies: PV-A reads 0.44 m per station on the same data, so the floor of the BED is ~0.4 m and PV-B's 1.79 m is
the CHAIN-ERA difference card CE is bisecting.  Two arms and one read are added below.

