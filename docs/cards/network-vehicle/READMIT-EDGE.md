# READMIT — the re-admission edge — `readmit_seed_horizon_s` promoted, the coupling-marginalize and handover-ratio halves KEEP_OFF

**STATUS**

- **id:** READMIT
- **topic:** network-vehicle
- **status:** closed
- **summary:** the re-admission edge — `readmit_seed_horizon_s` promoted, the coupling-marginalize and handover-ratio halves KEEP_OFF
- **dates in the text:** 2026-09-05 → 2026-09-06
- **knobs / posture:** `network.readmit_seed_horizon_s`, `network.readmit_coupling_marg_ep`, `network.phase_datum_handover_max_var_ratio`, `network.phase_leveling_local_block_min_age_ep`, `network.phase_leveling_local_block_young_mode`
- **hashes named in the text** (commit / exe / byte-cert): `808a0f91addee887`, `808a0f91`, `b7cea2adc8fc6fb6`
- **run dirs named in the text:** `runs/nonint_losblock3h_hv`, `runs/nonint_losblock24h_hv`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` — §6.10, §6.11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.10 The RE-ADMISSION EDGE card — BUILT 2026-09-05 (E2 seed-first re-admission + the block re-coupling window + the honest-σ-scale handover ceiling); PRE-REGISTERED, arms running

**What §6.9 named as the next card is built, default-OFF, byte-identical OFF, counted at both edges.**
Three knobs, one mechanism family (the satellite re-admission edge — the invariant-#7 edge that
§18.1 (T3 clock tails), §6.9 (the F3 24 h post-eclipse DD episodes) and §18.12 (the G20/E31 seam
residual) each measured from a different instrument):

| knob | what | where |
|---|---|---|
| `network.readmit_seed_horizon_s` (s, 0 = OFF) | E2 seed-first re-admission: a departing satellite's D2 **clock + code-bias** carry is PARKED (`CarryPrior::t_park_s`, walked by every frame re-base that walks `prev_carry`, dropped at a clock-datum change of its constellation, never serialized) and on re-registration inside the horizon SEEDS the fresh column at `var + q·gap`; the Q3 anchor folds ANCHORED over its true per-satellite gap (bounded by the horizon instead of `clock_anchor_max_gap_s`) and its NIS guard is the mark-if-moved (a trip within 300 s of the seed is counted as the data rejecting it).  The phase-bias family is deliberately NOT seeded — its frame is re-realized at the incumbent-less cold re-pin (the leveling carries do not survive a 55 min absence at the 120 s leveling gap) and a stiff seed would fight the pin. | park: `networkStashCarry` tail; seed: `networkInjectD2CarryPrior` (both D2 branches); fold bound: `clockAnchorFoldVariance(…, seed_horizon_s)`; attribution: `NetworkClockAnchor.cpp decouple()`; seed-vs-solve check at the stash (`READMIT seed_held/seed_moved`) |
| `network.readmit_coupling_marg_ep` (epochs, 0 = OFF) | the block/row RE-COUPLING edge: for K epochs after a satellite's clock column returns from a ≥ 3-loop-epoch absence, every Card-1 coupling term on its shared columns takes the DEPARTED route (σ² += M²·P_prev, coefficient dropped) in every station's rows and F3 block.  Both edges evented (`READMIT readmit` / `marg_expired`); the diverted-term count is the run-end denominator. | detector + window: `networkStashCarry` tail; consumer: `CouplingRowBuilder::readmitDiverted` (scalar, F3 block and the Card-1b joint path) |
| `network.phase_datum_handover_max_var_ratio` (0 = OFF) | item 15: a ledger value is usable only if `var ≤ ratio × var_first` — the arc's FIRST-STASH conditional variance (`LocalCarry::var_first`: set at birth, kept across re-stashes, restarted by an erase-and-rebirth, kept by a re-level); unknown ⇒ refused (the 378-σ guard stays armed); the absolute ceiling stays a rail; refusals counted on `handover_refused_ratio`; every PBGAUGE handover row carries `var`/`var_first` when armed (the item-15 prerequisite). | `networkElectPhaseAnchors` (`ratio_ok`); `var_first` at the leveling stash + the handover-ledger harvest; optional trailing L-row field in the snapshot (armed only) |

Unit contract: `NetworkCouplingTest.ReadmitWindowDivertsThePresentColumnAndExpires`,
`ClockAnchorFold.SeededReturnFoldsAnchoredOverTheTrueGap`,
`PhaseDatumHandover.RatioCeilingRefusesUnconvergedAndUnknownArcs`,
`PhaseDatumHandoverLoader.RatioCeilingDefaultsOffParsesAndRejectsNegative`,
`ReadmitContinuityConfig.*`, `ResumeFrameRestoreTest.DenseCarryCovAndVarFirstRoundTripUnderV3`
(the §18.12 (2a) half, see SPEC_synthetic §18.13).  Ledger rows added as UNVALIDATED; posture rows
SCOPED; neither knob fingerprinted (documented reasons in the loader/ledger).

**Arms (Release, `gnss_processor_readmit.exe` sha256 808a0f91addee887…, sequential, own CWD):**

| arm | config | single change vs its base |
|---|---|---|
| `readmit_off3h` | `_readmit_off3h.json` | the F3 3 h base (`_nonint_losblock3h_hv`) on the NEW exe, every new knob OFF |
| `ratio3h` | `_ratio3h.json` | + `phase_datum_handover_max_var_ratio 0.01`, absolute ceiling widened to a 1 m² rail |
| `readmit24h_e2` | `_readmit24h_e2.json` | the 24 h F3 arm (`_nonint_losblock24h_hv`) + `readmit_seed_horizon_s 3900` + `readmit_coupling_marg_ep 20` |
| `cplreg_q12warm_d` | `_cplreg_q12warm_d.json` | §18.12 arm C (frame + coupling restore, same v3 `.ep1440`) + the two readmit knobs |

**PRE-REGISTERED (written 2026-09-05 19:15 PDT, before any arm was read):**

1. `readmit_off3h`: `products.txt` sha256 **identical** to `runs/nonint_losblock3h_hv/products.txt`
   (invariant #6).  Falsifier: any byte differs — then nothing below is interpretable until the
   OFF path is repaired.
2. `readmit24h_e2` (the §7 FIRST criterion, verbatim): **no clean-chord degradation after the
   h16.6–18.9 re-admission cluster** — `readmit_grade.py` clean-chord L1C med|e| in h18–h21 ≤ 0.25
   cyc (base F3: 0.45–0.58); **settled quarter L1C ≤ 0.2 cyc / ≥ 80 %** (base 0.425 / 53 %); the
   quiet hours unchanged (h13–16 mature 0.16 / 89 %); lanes unchanged (WL ≥ 98 %, EWL 100 %).
   Counters (invariant #4): `n_readmit_events` ≈ the eclipse-return count (order 50–60 over 24 h
   for ~30 exclusion edges / 12 h), clock seeds > 0 with `n_readmit_seeded_anch` > 0,
   `n_readmit_seed_moved` < 10 % of clock seeds, `n_cpl_readmit_diverted` > 0.  Consumer h3–h7 ≤ 0.5
   m (base F3 1.1–1.8 m; the pre-F3 base 0.12–0.48).  Falsifiers: settled quarter > 0.3 cyc;
   `seed_moved` > 50 % of seeds (the parked realization is wrong more often than right); a
   degradation episode NOT present in the base.  A settled quarter between 0.2 and 0.3 cyc with the
   episodes visibly shortened is a PARTIAL (the edge is the right target, the seed/window sizing is
   not) and gets one follow-up arm, not a verdict.
3. `ratio3h`: settled window (last 25 %) handovers fully continuous (the 1e-2 arm: 21/21), **0
   settled PB frame steps > 1 m**, every refusal on an arc with `var/var_first > 0.01` (by
   construction — read the PBGAUGE rows), DD grade within noise of the 1e-2 arm (0.187 cyc / 83.1
   %), consumer settled 3D rms within 10 % of 0.245 m.  Falsifier: the settled window loses
   continuity (refusals on settled arcs) or the consumer walks.
4. `cplreg_q12warm_d`: the G20 (306240) and E31 (306450) `gap` trips do NOT fire (both seeded;
   `cta_decouple reason=gap` count over the leg 15 → ≤ 2), G20/E31 per-sat clock error at 309630 ≤
   0.3 m (arm C −2.25 / −1.12), **consumer h12 rms ≤ 1.0 m** (arm C 2.05, B 0.67, A 0.77 — the
   §18.12 stop rule 1 satisfied), wire between-sat SD at the seam unchanged vs C (0.025 G / 0.030
   E).  Falsifier: a seeded satellite trips `nis` within 300 s of its seed on ≥ half the returns.

Results go in §6.11 as they land; nothing above is edited after the fact.

**Addendum, arm 5 (written 2026-09-05 21:05 PDT, after arm 2's DD grade and BEFORE arm 5 was run).**
Arm 2 (`readmit24h_e2`) returned the base's DD to the digit (full 0.281 / 64.3 %, settled 0.420 /
53.4 % vs 0.283 / 64.2 % and 0.425 / 53.2 %) with the returned satellite's clock seeded AND its
columns marginalized out of every block for 20 epochs (2.98 M terms diverted) — so the post-eclipse
episodes are NOT the shared border.  By elimination the path is LOCAL: the legs BORN at a satellite's
re-admission (every station sees the returning satellite at once: ~20 stations × B bands of fresh
legs per return) carry their code-level birth error into the block, and the joint whitening L⁻¹
spreads it into the station's mature columns at their tight variance — §3.1's birth-frame
inheritance, amplified by F3 (which is why F1's diagonal control showed a smaller bump, §6.9).
Built: `network.phase_leveling_local_block_min_age_ep` — a leg younger than K loop epochs since its
first stash (`LocalCarry::birth_ep`) is kept OUT of the block and emitted as a per-column row
(F1-scaled over the young set when los_scale is armed); counted (`pl_blk_young_cols`).

| arm | config | single change vs its base |
|---|---|---|
| `readmit8h_young` | `_readmit8h_young.json` | the 24 h F3 posture (`_nonint_losblock24h_hv`) cut to 8 h + `phase_leveling_local_block_min_age_ep 30`; control = the first 8 h of `runs/nonint_losblock24h_hv` |

PRE-REGISTERED: after the h3.3–6.9 re-admission cluster the clean-chord L1C med|e| in h6–h7 stays
≤ 0.25 cyc (base 0.42 / 0.49) and the h6–h7 all-chord med|e| ≤ 0.30 (base 0.42 / 0.49); the quiet
hours h2–h3 unchanged within ±0.03 cyc (base 0.19 / 0.20); lanes unchanged; `pl_blk_young_cols` > 0
and rising at each return.  Falsifiers: h6–h7 > 0.35 cyc (the young legs are not the path either),
or h2–h3 worse than 0.25 (the gate costs the quiet state).  A partial (h6–h7 0.25–0.35) gets one
K-sweep arm, not a verdict.

**Arm 5 RESULT + arm 5b pre-registration (written 2026-09-05 22:45 PDT, after arm 5 was read and BEFORE arm 5b
was run).**  Arm 5 (`readmit8h_young`, young legs routed as per-column DIAGONAL rows) drove EVERY hour of the 8 h
window back to the pre-F1 over-count — L1C med|e| 3.78 / 2.58 / 2.58 / 2.70 / 2.31 / 2.27 / 1.99 / 2.35 cyc, 10–16 %
at the integer, WL 40–51 % (base F3 0.19–0.49 / 74–82 % / 96–99 %), quiet hours included, clean and touched chords
alike (228,423 young columns routed per-column over 55,440 blocks).  The falsified premise is "a diagonal row for a
SUBSET of a station's legs is harmless": it is not — §3.1's station-common over-count (rx clock / ZWD / rx-DCB
asserted ~n·B-fold by diagonal rows) re-enters through the ~15 % young rows and freezes the station level for
mature and young legs alike.  Corollary, recorded: the F3 factorization FALLBACK (F1/diagonal rows for that
station-epoch) is harmful for the same reason (0 fallbacks on every arm so far — latent).
Built: `network.phase_leveling_local_block_young_mode` = "free" (default; a young leg is NOT injected until it
matures — no memory, re-solved from its own epoch's data, joins the block at age K with its then-current
conditional variance) | "diagonal" (arm 5's route, kept for the record, loader-warned as measured harmful).

| arm | config | single change vs its base |
|---|---|---|
| `readmit8h_youngfree` | `_readmit8h_youngfree.json` | arm 5 + `phase_leveling_local_block_young_mode "free"` (K = 30, 8 h); control = the first 8 h of `runs/nonint_losblock24h_hv` |

PRE-REGISTERED (arm 5b): the arm-5 criterion verbatim — clean-chord L1C med|e| in h6–h7 ≤ 0.25 cyc (base 0.54 / 0.48
clean; 0.42 / 0.49 all), h2–h3 unchanged within ±0.03 (base 0.19 / 0.20), lanes unchanged, `young column(s) kept
out of the block` > 0.  Additional falsifier specific to "free": the young-leg population's own med|e| (the
mature/young table) must not exceed 1.0 cyc at h2–h3 (base young 0.74 / 0.64) — a free young leg has one epoch of
data, and if that reads worse than the diagonal-carried young leg the trade is a loss even if the mature chords win.

## §6.11 Results of the §6.10 arms (filled as they land; nothing in §6.10 is edited)

> ⚠ **Read with §6.12 (2026-09-06):** every DD number below is AS-TRACED against the trace's lexically-lowest reference satellite; the "post-eclipse network-wide DD episode" that items 2, 6 and 7 chase is that reference (G01 / E02) returning from eclipse.  The ref-free re-grades are in §6.12.3.


1. **`readmit_off3h` — invariant #6 PASS (2026-09-05 19:22 PDT).**  New exe `gnss_processor_readmit.exe`
   (sha256 808a0f91…), every new knob at its default: `products.txt` sha256 **b7cea2adc8fc6fb6…**, 8,593,106
   bytes — identical to `runs/nonint_losblock3h_hv/products.txt`.  The OFF path of all four increments is
   byte-inert on the F3 3 h lineage; the arms below are single-family A/Bs against that base.

2. **`readmit24h_e2` (E2 seed 3900 s + coupling window 20) — CLOCK HALF PASS, DD HALF FALSIFIED (20:45 PDT).**
   Counters: 28 re-admissions; 110 entries parked; 26 clock + 84 code-bias seeds (26 clock seeds folded
   ANCHORED); 0 expired / 0 datum-dropped / 0 unconsumed; Q3 trips within 300 s of a clock seed 8 (5 in-epoch at
   the return on the T3 satellites G25/G26/G16/G12 with e 0.18–0.37 m, 3 one epoch later at 5–18 cm); seed checks:
   clock 21 held / 5 moved, C1C 28/28 held, C2W 16/16 held, **C5Q 12 held / 16 moved, C7Q 0 held / 12 moved**
   (the third-frequency code biases re-realize at every return — the §18.1 cold-start C5Q misrealization carried
   through a stiff σ 1.9 cm seed); `gap` trips 26 (base) → 0; 28 coupling windows, 2.98 M terms diverted.
   WIRE (per-sat clock census, settled half, truth): **G25 6.27 → 1.69 m, G12 5.38 → 2.71, G26 6.51 → 4.34** — the
   T3 tails; every other satellite within ±0.2 m.  CONSUMER (hourly rms, E2 / base F3 / pre-F3 control): h3 0.90 /
   1.81 / 1.23; h4 0.54 / 1.10 / 0.98; h5 0.54 / 1.19 / 0.48; h6 0.50 / 1.40 / 0.12; h7 0.32 / 1.11 / 0.12; h8 0.15 /
   0.47 / 0.07; h12–23 0.05–0.13 vs 0.06–0.19 vs 0.05–0.15 — the post-cluster excursion cut 2–3× (pre-registered
   ≤ 0.5 m at h3–h7: met at h6–h7, not h3–h5).  **DD: full 0.281 cyc / 64.3 % (base 0.283 / 64.2 %); settled
   quarter 0.420 / 53.4 % (0.425 / 53.2 %); every hourly episode identical** (h6 0.442 / h7 0.480 / h19 0.581 / h20
   0.557 vs 0.422 / 0.485 / 0.585 / 0.561; clean-chord h6 0.545, h19 0.654).  The pre-registered settled ≤ 0.2 /
   ≥ 80 % is NOT met, and the mechanism §6.9 named — the returned satellite's shared columns entering the blocks —
   is FALSIFIED: with those columns marginalized out of every block for 20 epochs AND the clock seeded, the DD did
   not move by a digit.  Verdict: `readmit_seed_horizon_s` = the T3 fix on the wire; the code-bias half of the seed
   is wrong for third-frequency signals (sub-knob `readmit_seed_code_bias`, arm `readmit24h_e2clk`);
   `readmit_coupling_marg_ep` = inert on the DD (no harm measured) — keep OFF pending a target.

3. **`ratio3h` (ratio 0.01, absolute rail 1.0 m²) vs the 1e-2 arm — MECHANISM VALIDATED, VALUE 0.01 FALSIFIED.**
   Handovers 53 continuous / 40 cold (base 70 / 23); the 40 refusals are exactly the r = var/var_first > 0.01
   population (0 by the rail).  Settled window 18 / 3 (base 21 / 0): the 3 refusals are ONE family at ONE epoch —
   E25 L1C/L5Q/L7Q at 269280 (S01000SIM → S09000SIM), var 2.8e-3 / 9.0e-3 / 8.2e-3 m² (all UNDER the 1e-2
   absolute), var_first 0.075 / 0.238 / 0.215 m² → r = 0.038; every other settled handover had r in
   0.0003–0.0084.  Cost: the cold re-pin put a **7.6 m phase-bias frame step** on the wire at 269280 (base:
   0.03 m, 0 keys) — the pre-registered "0 settled PB steps > 1 m" FAILED.  DD 0.196 / 82.1 % (base 0.187 / 83.1 %)
   and consumer 0.223 vs 0.243 m: no harm elsewhere.  Reading: the instrument is exact and both edges are counted,
   but the DENOMINATOR is biased for an arc that spent time PINNED as the anchor — a pinned arc is never carried, so
   its "first stash" lands after the pin moves away with an already converged variance (E25 @ S09000SIM: 0.075 m²,
   σ 27 cm, vs the ~0.8 m² code-level birth of a fresh arc) and the ratio reads a converged, harmless successor as
   unconverged.  KEEP the absolute 1e-2 as the working value; the ratio form needs a birth variance harvested at the
   arc's FIRST SIGHTING (pinned or not) before it can replace it.  Ledger: KEEP OFF, denominator card owed.

4. **`cplreg_q12warm_d` (seam leg: arm C + E2) — E2 INERT on the seam consumer; §18.12's attribution corrected.**
   See SPEC_synthetic §18.14.

5. **`frwriter_sticky_v3c` — WRITER GATE PASS.**  products.txt 34,840,533 bytes, a byte-identical prefix of the
   continuous run; `.ep1440` v3 with QK 376 / QP 376 / QL 2811 (16.8 MB); history pruned to the branch point.

6. **`readmit8h_young` (arm 5: young legs as per-column DIAGONAL rows, K = 30) — FAIL, and a finding.**  L1C
   med|e| 3.78 / 2.58 / 2.58 / 2.70 / 2.31 / 2.27 / 1.99 / 2.35 cyc (h0–h7), 10–16 % at the integer, WL 40–51 % —
   the ENTIRE solution back at the pre-F1 over-count, quiet hours included, clean and touched chords alike
   (228,423 young columns over 55,440 blocks).  The falsified premise is "a diagonal row for a SUBSET of a
   station's legs is harmless": §3.1's station-common over-count re-enters through the ~15 % young rows and freezes
   the station level for every leg.  Corollary: the F3 factorization FALLBACK (diagonal rows) is harmful for the
   same reason (0 fallbacks on every arm so far — latent).  Response: young legs FREE (arm 5b).

7. **`readmit8h_youngfree` (arm 5b: young legs FREE until age 30).**  FAIL — the newborn-leg hypothesis is falsified too (23:05 PDT).  Hourly L1C (5b / base): h2 0.221 / 0.192, h3 0.214 / 0.198, h4 0.308 / 0.232, h5 0.350 / 0.271, h6 0.554 / 0.424, h7 0.588 / 0.485; WL 84–92 % (base 96–99 %); clean chords h6 0.761 / h7 0.616 (base 0.538 / 0.478) — the episode LARGER, not smaller; mature legs h2–h3 0.175 / 0.169 (base 0.160 / 0.167); free young legs 1.7–2.0 cyc (base carried young 0.74 / 0.64, the additional falsifier hit); 232,089 young columns kept out of 55,440 blocks.  Removing the newborn legs from the block does not protect the mature chords through the cluster.  With arm 2, the post-eclipse DD episodes are NEITHER the returned satellite's shared columns NOR the newborn legs' block membership; both age-gate responses fail → `phase_leveling_local_block_min_age_ep` KEEP OFF (free = harmless but useless, diagonal = harmful).  The base's event census through the h16–h21 episode shows no datum / era / withhold events (anchor handovers 12–33 and ~20 T1 refsat_resets per 20 min, 7-column col_added/removed per return) — the next card is a fresh investigation of what re-realizes NETWORK-WIDE at a re-admission cluster that the exactly-conditioned F3 block resists (prefit census / topology ledger / rhs_attrib), not another gate.

8. **`readmit24h_e2clk` (clock-only seed).**  FALSIFIED as an improvement (23:55 PDT).  26 clock seeds, 0 code-bias seeds; seed checks 20 held / 6 moved; 7 seed-attributed Q3 trips (arm 2: 8).  WIRE (settled-half per-sat clock): G12 6.26 m (arm 2 2.71, base 5.38), G26 6.15 (4.34, 6.51), G25 5.63 (1.69, 6.27) — the T3 tails RETURN.  CONSUMER h3–h8 1.76 / 0.71 / 0.66 / 0.78 / 0.34 / 0.13 m (arm 2 0.90 / 0.54 / 0.54 / 0.50 / 0.32 / 0.15).  DD unchanged (0.287 / 64.0 %, settled 0.430 / 53.0 %).  The IF-pair code-bias seeds anchor the clock-vs-code-bias SPLIT at the return, so the clock lands near the pre-gap realization even when its own seed is rejected; without them the cold third-frequency misrealization relocates into the clock — §18.1's T3 mechanism.  The third-frequency seed rejections are the stash-side check working, not harm.  `readmit_seed_code_bias` stays TRUE (default); `readmit_seed_horizon_s` 3900 s (both kinds) is the validated T3 fix → PROMOTED: ledger RECOMMEND_SCOPED (Q3-anchored lineages without clock_model, armed_when > 0), posture row, sim golden pin 3900 (manifest, regenerated golden, verify PASS, 20/20 smoke).

