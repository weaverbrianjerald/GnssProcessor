# RF-3-read: the IF-correction steps at held-basis handovers, and RF-2d (2026-09-15)

**STATUS**

- **id:** RF-3-read
- **topic:** network-vehicle
- **status:** closed
- **summary:** The vehicle's IF-correction dispersion is satellite clock stepping at cold held-basis handovers (two RF-2b defects); RF-2d fixes both: sim consumer PASS, field FAIL (573 / 720 emitted).
- **dates in the text:** 2026-09-15
- **knobs / posture:** RF-0g + `network.ambiguity_vehicle` float_kf + `network.phase_datum_held_basis`; RF-2d knobs `network.phase_datum_held_cold_release` (solve-affecting) and `network.phase_datum_held_mark_cold` (wire-affecting), both default OFF and inert without the held basis; instruments `scripts/rf3_if_attrib.py`, `scripts/rf2d_grade.py`
- **hashes named in the text:** exe `gnss_processor_rf2d.exe` `0D7BA3548BB23D2D` (main `4a9f5de` + the two knobs); OFF smoke `9add1d3ab9d70657`, record stream `333dcb0871e1f5bc`; suite 2217 / 292 (2216 passed, 1 fixture skip)
- **run dirs named in the text:** `runs/_rf3p_{sim,field}6`, `runs/_rf2d_sim6_{mark,rel,both}`, `runs/_rf2d_field6_both`, `runs/_rf2d_smoke_off`, `runs/_rf0g_{sim,field}6`
- **source:** [RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md) section 5 (the read as pre-registered)

**Outcome.** The question: what moves the per-satellite IF correction under the float vehicle. Its steps are satellite clock (code-bias part 0.000 m, so the GF-bias direction is not the term); RF-2b's mark predicate is inverted (144 of 145 continuous handovers marked) and RF-2 clears the cold families' retired-frame release. RF-2d: declarations PASS on both beds; the mechanism bar missed (an ignition event per bed, field dispersion p90 1.007 m); the sim consumer PASS; the field FAIL (hour 2 at 3.00 m), so the vehicle was not back in contention. Both knobs are armed in the goldens since [GRB](GRB_golden_rebase_2026-09-22.md); [RF-NS](RF-NS_null_space_read_2026-09-15.md) read the field withholds.

**Status as recorded (2026-09-15):** closed — **RF-2d RESULT (§7): the two defects are fixed and the SIM passes its consumer bar decisively** (both knobs: sim cold
consumer 3.14 / 0.47 / 0.24 / 0.24 / 0.28 / 0.33 m against the golden's 6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55 m — the best sim consumer on
the self-contained posture; IF dispersion p90 0.156 → 0.043 m; every declaration correct; the release carries the win, the mark fix the
hour-6 tail).  **NOT back in contention on the field:** settled hours 3–6 beat the golden (0.84 / 0.65 / 1.37 / 0.60 m against 1.28 /
1.25 / 1.78 / 2.68 m) but 573 / 720 emitted (the S-basis null-space gate withholds 67 epochs after hour 1 that the golden does not), hour 2
3.00 m and IF dispersion p90 1.01 m.  Knobs default OFF, OFF smoke byte-identical.  Next pre-registered (not launched): the null-space read.
The read itself, as pre-registered: (1) The dispersion RF-3-prep measured
(field p90 0.51 → 3.03 m, sim G29 56 m) is **100 % satellite CLOCK**: the code-bias part of every de-commoned step is 0.000 m on all runs,
so the geometry-free-bias direction the frozen reading named is **not the term**.  (2) It **steps at the vehicle's own events — the COLD
held-basis handovers**: on the sim 5 of 6 cold handovers (all five undeclared ones) step the IF correction by 19–65 m while 50 of 50 continuous ones move it < 0.01 m.
(3) The mechanism is **two defects in RF-2b's handover bookkeeping**, not physics: the declaration predicate `|value − prev_value|` is
inverted against the frame (field: 144 of 145 continuous handovers MARKED, 10 of 12 cold ones UNMARKED), and RF-2 clears the cold families'
retired-frame release (`ApplicationNetwork.cpp`, the `pa_reref.clear()` under `pd_held_basis`), so the cold step folds against stale
priors and the free satellite clock absorbs it.  (4) The level-vehicle control keeps that release and its cold handovers do not step
(RF-0g sim: 27 cold, IF-step p90 0.002 m).  Instrument `scripts/rf3_if_attrib.py`; one-command grade `scripts/rf2d_grade.py`.

## 1. The instrument

`scripts/rf3_if_attrib.py` reads `products.txt` and forms per satellite the correction a consumer applies, `x = −clk + a1·b(C1C) + a2·b(C2W)`
(`refound_grade.if_dispersion`'s quantity).  Each epoch-to-epoch change is de-commoned by the median change over the satellites present at
both epochs (the common part cancels in the consumer's clock), split into its clock part and its code-bias part, and called a STEP above
0.25 m.  Every step is matched to that satellite's `network_events.csv` rows (and `network_amb_state.csv` drops / erases, flagged when on the
anchor) in (t_prev, t], and the match rate is compared with the same rate over the non-step pairs — at event density a count attributes
nothing, a lift does.  `--handovers` tabulates the step at every held-basis handover by per-satellite-epoch MODE (all-continuous / all-cold /
mixed; `+cold_unmarked` = an `sts_cold_declared` row with `marked=0`) against the no-handover null.

## 2. What moves it

**Field RF-3p, the dispersion's own span (mid hour-2 → mid hour-6 windows):** 69 steps, Σ|dy| 74.9 m — clock 74.9 m, code bias 0.0 m.  Steps
with any event of their satellite: 78 % against 17 % base.  Lifts: `CLK_ANCHOR cta_decouple` 243×, `READMIT` 120×, `T4_attrib` refloat (=
`AMB drop:t4_attrib`) 80×, `AMB drop:stale_gap` 24×.  The worst satellites are single events: G03 −7.39 m in one step at +266 min (T4 drop +
anchor handover); G20 −9.42 then +8.01 m at +224 min (T4 + clock decouple + handover), −2.70 m at +265 min; G27 +3.18 m at +265 min.  The
+265 min cluster is the resume after a **49-epoch withheld stretch (TOW 274859–276299) that RF-0g does not have** — every carry of those
satellites goes stale (`drop:stale_gap`, `erase:gap_prune`) and the clocks re-realize.  Same span on RF-0g: 28 steps, 19.0 m.

**The mode table** (6 h; |dy| = the larger de-commoned step at the handover epoch and the next):

| run | mode | n | p50 m | p90 m | max m | > 1 m |
|---|---|---|---|---|---|---|
| RF-3p sim | all-continuous | 50 | 0.001 | 0.002 | 0.01 | 0 |
| | all-cold (marked) | 1 | 0.005 | 0.005 | 0.01 | 0 |
| | all-cold + cold_unmarked | 3 | 19.06 | 24.97 | 26.45 | **3** |
| | mixed + cold_unmarked | 2 | 40.25 | 60.23 | 65.22 | **2** |
| RF-3p field | all-continuous | 76 | 0.001 | 0.079 | 6.43 | 3 |
| | all-cold (marked) | 2 | 6.30 | 8.79 | 9.42 | **2** |
| | all-cold + cold_unmarked | 18 | 0.30 | 7.33 | 18.54 | **7** |
| | mixed + cold_unmarked | 3 | 0.07 | 4.05 | 5.04 | 1 |
| RF-2b sim (GIM) | all-continuous / cold_unmarked | 50 / 3 | 0.001 / 0.27 | — / 0.47 | — / 0.52 | 0 / 0 |
| RF-2b field (GIM) | all-continuous / all-cold (+unmarked) | 98 / 7 | 0.001 / 0.03 | 0.006 / 0.59 | 2.25 / 0.95 | 2 / 0 |
| **RF-0g sim (level, release kept)** | all-cold | 27 | 0.000 | 0.002 | 1.24 | 1 |
| **RF-0g field (level, release kept)** | all-cold | 24 | 0.002 | 0.55 | 18.54 (ignition, +0 min) | 2 |

Declarations (STRANSFORM rows, 6 h): RF-2b field continuous marked 144 / unmarked 1, cold marked 2 / **unmarked 10**; RF-2b sim 95 / 1, 0 /
**5**; RF-3p field 120 / 1, 4 / **27**; RF-3p sim 100 / 1, 2 / **8**.

## 3. The traced case — sim G29 at TOW 267690 (+142 min; the sim consumer's 12.98 m hour)

G29 is carried at three stations (S29 the anchor, S09, S02).  S29 sets and both families hand over to S09 in one epoch: **L1C continuous**
(target = S09's carried level in the outgoing frame, −27.157 m; `sts_applied` Δ −27.157, marked) and **L2W COLD** (S09's carried level
−51.543 m refused by the variance ceiling; pinned at 0; `sts_cold_declared` basis 0, prev 0, **marked 0**, because S29's own L2W tenure had been
cold at 0).  The L2W family therefore re-realizes by ≈ 51.5 m, undeclared.  S02's carried G29 ambiguities do not move (L2W +0.08 m) — the
retired-frame priors stay in force.  The Q3 anchor's NIS reads 159 against 6 (`cta_decouple`), and the emitted clock goes +3.41 → −61.81 →
−0.41 → +2.41 → −58.81 → −59.82 m over five epochs (each a declared `CLK_STEP` retro bump) and parks near −60 m for the rest of the run, the
B records unmoved.  a2·51.5 m = 79.7 m of IF phase datum, most of it absorbed by the one state with no prior left.

## 4. The two defects, in the frame algebra

Let anchor a hold the family at v_a, and let L_k be successor k's level in that frame (its ledger `value_abs`), with error e_k (σ ≤ √var_k).
- A **continuous** handover pins k at L_k: the family's frame moves by e_k only (≤ 0.1 m at the armed 0.01 m² ceiling; measured < 0.01 m on the
  IF correction).  But `value − prev_value` = L_k − v_a, the between-station level difference, which is essentially never 0 — so RF-2b marks
  every continuous handover and the consumer resets satellites whose wire did not move.  The code comment asserts "Δ = 0 by construction";
  the construction gives Δ_wire = 0, not Δ_ledger = 0.
- A **cold** handover pins k at 0: the family's frame moves by L_k.  `value − prev_value` = −v_a, which reads 0 whenever the outgoing tenure
  was itself cold — so the one handover the consumer must hear about is the one that goes silent.  The size the wire actually steps, L_k, is
  not what the row declares even when it does mark.
- The estimator side: R2 narrowed `phase_anchor_reref` to exactly the cold and first-sighting families and releases their retired-frame
  carries (the shared PB carry for one epoch, the local ambiguity carries at every station) so that "the frame step lands on the FREE phase
  bias instead of being absorbed by the satellite clock (measured 41 sigma on G21)".  RF-2 clears that set on every held-basis election on the
  premise that "the carries are not stale, they are expressed against a datum whose move the consumer has been told about" — true for a
  continuous handover, which never entered the set; false for everything that did.

## 5. What this revises, and what it does not

- RF-3-prep §2's reading ("with the GIM gone nothing but the T_GD pin separates the float ambiguity from the geometry-free bias and the slant
  iono") is **not the mechanism of the dispersion** — the steps carry no code-bias part.  The KEEP OFF verdict on RF-1 + RF-2b stands (the
  vehicle as built fails); its cause is re-attributed to §4.
- RF-2b's "marks inside the re-based bar" counted the false continuous marks as declared handovers; the bar was met by the defect.
- **Not separated here:** why the same cold-unmarked handovers moved the GIM runs' IF correction only ≤ 0.52 m (candidates: more carrying
  stations per satellite before the X / roster cuts; more successors refused by the ceiling without the GIM, so more cold handovers mid-arc
  against mature priors); and the cause of RF-3p field's 49-epoch withhold at TOW 274859.  Both are read after RF-2d, on its arms.

## 6. RF-2d — pre-registered (before any run)

**Knobs** (both default OFF, byte-identical; INERT unless `network.phase_datum_held_basis`; loader warns when set without it):
- `network.phase_datum_held_cold_release` — keep the reref set under the held basis (the cold / first-sighting families are released exactly
  as on the level vehicle).  Solve-affecting.
- `network.phase_datum_held_mark_cold` — declare a held-basis handover iff it is cold.  Wire-affecting.

**Arms** = the RF-3p arms (RF-0g + float_kf + held basis) + the knob(s), 6 h cold, same exe: sim `_rf2d_sim6_mark`, `_rf2d_sim6_rel`,
`_rf2d_sim6_both` (the attribution: each knob alone, then both); field `_rf2d_field6_both` only if the sim's both-arm passes M1 and M2.
Controls: `runs/_rf3p_{sim,field}6` (no fix), `runs/_rf0g_{sim,field}6` = the goldens.  OFF: the canonical smoke must reproduce
`9add1d3a…` / record stream `333dcb08…`.

**Bars** (grade `python scripts/rf2d_grade.py <runs>`; consumer numbers re-read from `closed_loop_trace.csv`, in metres):
- **M1 mechanism** (rel, both): no cold held-basis handover with an IF-correction step > 1 m; IF dispersion p90 ≤ the golden's (sim 0.156 m,
  field 0.512 m).
- **M2 declaration** (mark, both): continuous marked = 0 and cold unmarked = 0.
- **C1 sim consumer** (both): every hour from hour 3 ≤ RF-0g + 0.10 m (0.86 / 0.50 / 0.46 / 0.55 m), hours 1–2 ≤ RF-3p's (6.66 / 1.69 m).
- **C2 field** (both): emitted ≥ 640; consumer hours 2–6 ≤ RF-0g + 0.5 m (1.08 / 1.28 / 1.25 / 1.78 / 2.68 m).
- **Verdict rule:** M1 + M2 ⇒ the two defects are fixed (the knobs stay, default OFF, as the correct held-basis semantics); M1 + M2 + C1 +
  C2 ⇒ the vehicle is back in contention on the golden posture and a golden arm is proposed to the author (no pin without the author's go).
  M1 fails with the release armed ⇒ the retired-frame release is not the whole estimator side (read the PB carry and the S02-type carried
  priors at the step epochs before any further build).

## 7. Results

### 7.1 Build and identity (2026-09-15)

Exe `build/x64-Release/gnss_processor_rf2d.exe` sha256[:16] `0D7BA3548BB23D2D` (main `4a9f5de` + the two knobs).  OFF smoke
(`runs/_rf2d_smoke_off`, the RF-0g smoke config): products sha256 `9add1d3ab9d70657`, record stream `333dcb0871e1f5bc` — **byte-identical**
to the canonical hashes.  All five processes exit 0.  Debug `gnss_tests`: 2217 tests / 292 suites — 2216 passed, 1 fixture skip.

### 7.2 The table (6 h cold; consumer = p50 by hour from `closed_loop_trace.csv`, metres; `python scripts/rf2d_grade.py <runs>`)

| arm | emitted | consumer p50 by hour (m) | consumer p90 by hour (m) | IF disp p50 / p90 / max (m) | continuous marked / cold unmarked | cold handovers (sat-epochs, incl. mixed) > 1 m |
|---|---|---|---|---|---|---|
| SIM RF-0g (golden) | 720 | 6.23 / 3.30 / 0.86 / 0.50 / 0.46 / 0.55 | 8.47 / 4.46 / 1.22 / 0.74 / 0.86 / 0.79 | 0.050 / 0.156 / 0.72 | — | 1 of 28 (1.24) |
| SIM RF-3p (no fix) | 720 | 6.66 / 1.69 / 12.98 / 7.15 / 1.27 / 0.94 | 8.45 / 5.40 / 19.17 / 7.87 / 4.64 / 1.03 | 0.099 / 0.862 / 56.53 | 100 / 8 | 5 of 6 (to 65.2) |
| SIM `_rf2d_sim6_mark` | 720 | 6.37 / 1.45 / 13.38 / 7.17 / 0.94 / 0.48 | 8.45 / 4.76 / 19.22 / 7.89 / 4.35 / 0.63 | 0.099 / 0.862 / 56.53 | **0 / 0** | 5 of 6 |
| SIM `_rf2d_sim6_rel` | 720 | 3.14 / 0.47 / **0.20 / 0.22** / 0.30 / 0.49 | 5.72 / 0.61 / 0.29 / 0.28 / 0.48 / 0.55 | 0.022 / **0.043** / 0.10 | 98 / 9 | 1 of 6 (1.19) |
| **SIM `_rf2d_sim6_both`** | 720 | **3.14 / 0.47 / 0.24 / 0.24 / 0.28 / 0.33** | 5.72 / 0.93 / 0.29 / 0.29 / 0.45 / **0.38** | 0.022 / **0.043** / 0.10 | **0 / 0** | 1 of 7 (1.19) |
| FIELD RF-0g (golden) | 640 | 3.77 / 1.08 / 1.28 / 1.25 / 1.78 / 2.68 | 5.30 / 3.54 / 1.64 / 1.35 / 2.64 / 3.86 | 0.175 / 0.512 / 1.50 | — | 2 of 31 |
| FIELD RF-3p (no fix) | 564 | 1.49 / 2.96 / 2.60 / 3.15 / 2.02 / 2.65 | 3.61 / 3.72 / 3.34 / 3.61 / 4.88 / 3.20 | 0.661 / 3.026 / 7.46 | 120 / 27 | 10 of 24 |
| **FIELD `_rf2d_field6_both`** | 573 | 4.75 / 3.00 / **0.84 / 0.65 / 1.37 / 0.60** | 6.43 / 4.32 / 2.55 / 0.74 / 2.10 / 3.12 | 0.248 / 1.007 / 2.70 | **0 / 0** | 2 of 18 |

Clock re-realizations follow the release: sim `cta_decouple` 97 (RF-3p, mark) → 36 (rel, both — the golden's own count), `CLK_STEP`
34 → 25.  The G29 handover of §3 now moves the IF correction 0.000 m.  Field dispersion-span step budget (§2's instrument, mid hour-2 →
mid hour-6): 69 steps / 74.9 m (RF-3p) → **19 / 14.8 m**, below the golden's 28 / 19.0 m; the remaining field dispersion is smooth drift
(G20 −0.82 m, G28 −0.65 m) and the +265 min withhold resume (G27 +0.96 m), not handover steps.

### 7.3 The bars, literally

- **M1 mechanism.** (a) "no cold handover with an IF step > 1 m": **missed by one ignition event on each bed** — sim G15 at +3 min (1.19 m;
  the golden's own cold tail is 1.24 m), field G08 at the first emission (18.54 m, present on every arm including the golden) and G32 at
  +10 min inside the ignition withholds (1.50 m).  After ignition no cold handover steps the IF correction by more than 1 m on either bed
  (sim max 0.006 m).  (b) dispersion p90 ≤ the golden's: **sim PASS** (0.043 ≤ 0.156), **field FAIL** (1.007 > 0.512).
- **M2 declaration: PASS on both beds** (continuous marked 0; cold unmarked 0; sim 12 cold marked, field 24).
- **C1 sim consumer: PASS** (hours 3–6 0.24 / 0.24 / 0.28 / 0.33 ≤ 0.96 / 0.60 / 0.56 / 0.65; hours 1–2 3.14 / 0.47 ≤ 6.66 / 1.69).
- **C2 field: FAIL** — emitted 573 < 640; hour 2 3.00 > 1.58 m (hours 3–6 pass).
- **Verdict (the pre-registered rule):** M2 passes and M1's mechanism half passes outside ignition on both beds ⇒ **the two defects are fixed**;
  the knobs stay built, default OFF, as the correct held-basis semantics.  C2 fails ⇒ **the vehicle is NOT back in contention**; no golden
  arm is proposed.  Attribution: the RELEASE carries the clock and the consumer (mark alone = RF-3p to within noise); the MARK removes ~100
  false consumer resets per 6 h and is worth hour 6 on the sim (0.49 → 0.33 m p50, 0.55 → 0.38 m p90).

### 7.4 What leads on the field now, and the next read (pre-registered, not launched)

The withheld stretches of `_rf2d_field6_both` after hour 1 are the same TOWs as RF-3p's (274859–276299, 49 epochs; 279960–280470, 18) and all
are the **S-basis null-space gate** (`NetworkSbasis`: 48 epochs observe 2 near-zero eigenvalues and 19 observe 4, against a declared
dimension of 1) — the release does not create them.  The golden has none after hour 1, but its hour-1 ignition deficit is the same gate
(16 below cutoff at the first epoch).  **The null-space read:** from `network_estimability.csv` (the gate's eigen-census; `col1..col8` name
the dominant columns), at the vehicle's hour-5 and hour-6 stretches and the golden's first 75 epochs, name every direction below the cutoff
by its states (which satellite / station / family), and cross it with that epoch's handovers, releases, arc births and deaths.  Frozen
reading: the extra directions are families whose phase datum is momentarily unpinned (a held basis with no elected successor, or a thin
satellite whose only carrying stations just died) ⇒ a DECLARATION fix (declare them, pin or withhold per family, not the epoch); anything
else ⇒ a rank read of the float block at that geometry before any build.
