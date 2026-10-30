# RCG: the two keys that cut refloats in card BOF's triage, graded on 6 h arms

**STATUS**

- **id:** RCG
- **topic:** raim-qc
- **status:** closed
- **summary:** The two never-graded keys that moved the solve in card BOF's 2 h field triage (BOF section 3), graded as BOF section 5 asks: 6 h field arms and the 16-site census against a same-exe control on the golden of main `1527cf9` (the control byte-identical to `runs/_rf0hv17_field6`), and 6 h sim arms. `network.raim_refloat_maturity_gate` at 120 epochs PASSES every gate: T4 refloats 107 to 55, slip-class ambiguity responses -6.9 %, held-out consumer +0.01 mm over hours 2 to 6, census +0.06 mm with no site worse, byte-identical on the sim; it counts nothing it suppresses, a telemetry change owed before any arming. At 60 epochs the churn cut is -3.1 %. `network.signal_integrity.census` is HARMLESS, NOT LESS CHURN: T4 refloats 107 to 78 but slip-class responses -3.3 % (bar -5 %), consumer +0.10 mm, census +0.05 mm, sim +0.02 mm. Neither key moves a user; the triage's hour-2 gains do not reproduce. Gates in section 3, committed before any arm ran (`68b6a88`); results in section 4. Nothing is armed here: the author decides.
- **source:** [BOF](BOF_built_off_follow_through_2026-10-28.md) sections 3 and 5; worker report `D:/GnssProcessorSupport/runs_nom/bof_reports/triage_2h.md`; the "never graded" section of [OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md); ledger rows of the keys in `docs/CONFIG_promotion_ledger.json`
- **knobs / posture:** read, not changed: `network.signal_integrity.census` (UNVALIDATED; its PARAM children at their loader defaults: `unvetted_sigma_scale` 10, `revet_ep` 3, `flicker_max_ep` 5, `refloat_on_regain` true, `withhold_instead` false), `network.raim_refloat_maturity_gate` (UNVALIDATED) with `network.raim_refloat_maturity_epochs` (PARAM) 120 and 60. Bases: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `1527cf9`.
- **dates in the text:** 2026-10-29 (pre-registration), 2026-10-30 (results)
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/worktrees/mgsi/build/x64-Release/gnss_processor_rcg.exe` (the Release build of main `1527cf9` in a clean detached worktree, sha256[:16] fab88a924e9189fd); run root `D:/GnssProcessorSupport/runs_nom`: field arms `_rap_rcg_{ctl,sic,mg120,mg60}_f6`, sim arms `_rap_rcg_{ctl,sic,mg120}_s6`, census replays `_rap_rcgc_{ctl,sic,mg120,mg60}_<site4>` (log `rcg_census.log`)
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: do the two keys that cut refloats on one 2 h arm keep the users within their bars over 6 h, and
do they cut the refloat churn? Neither moves a user: the held-out consumer within +0.10 mm over hours 2 to 6, the census
within +0.06 mm with no site worse, the sim within +0.02 mm; the triage's hour-2 gains do not reproduce. The maturity
gate at 120 epochs halves the T4 refloats (the cut all in hour 1, no catch-up at the lift) and passes (-6.9 %), but
counts nothing it suppresses. The census cuts T4 refloats by 27 % and the slip-class responses by 3.3 %: harmless, not
less churn. Standing: both off, the author's decision ([register](../status/OPEN_ITEMS_BUILT_OFF.md)).

## 1. What each key changes, and its event edges

Standing invariant 7 of `CLAUDE.md`: a mechanism that changes which information reaches which states is an event at
both edges, with counters and identities.

### 1.1 `network.signal_integrity.census`

Seam `networkSignalIntegrity` (`src/core/network/assembly/SignalIntegrity.cpp`; design `docs/SPEC_flicker_gap_integrity.md`),
run once per epoch after the PASS A slip scans and the assembly prefilter, before the phase-datum election and PASS B. It
classifies each (station, satellite) line of sight as Stable, New, Reentry, FlickerRegain or Degraded (record
`net::LosState::si_arc`) and arms three actions with the master switch:

- **Demotion.** A line of sight that has completed a vet once in the run (`ever_stable`) and has had fewer than
  `revet_ep` (3) consecutive full-band epochs since its last transition (a re-entry, a reference band lost or regained,
  a band dropped from or added to the reference set) folds its phase rows at sigma x 10, a weight of 1 %. Code rows are
  untouched. An arc that has never converged (every arc at a cold start, a rising satellite) is exempt by rule.
- **Regain refloat.** A reference band that returns within `flicker_max_ep` (5) epochs while the line of sight stayed
  present resets every band of the line of sight: a slip-funnel insertion when `network.carry_invariant` is on (off in
  both goldens) and a pending-reseed mark when the carry is warm. On the goldens only the second feed acts (triage:
  37 regains, 0 funnel insertions, 57 pending reseeds).
- **Re-entries** are counted, not acted on: the design's premise that a detector already refloats a re-entry is
  measured.

Edges:

| edge | what happens | counters and events |
|---|---|---|
| demotion, decoupling | at the transition epoch the line of sight's phase drops to 1 % weight; while demoted its ambiguity and slant ionosphere take their information from code and the carried priors | `demoted_los_epochs`, `demoted_rows`; SIGNAL events `reentry`, `band_lost`, `band_regain`, `degraded` (one per window) |
| demotion, re-coupling | full weight returns after 3 consecutive full-band epochs; the vet length is the re-validation: the MW detector (two time-difference samples) and IF-TD (three epochs) are live for at least one epoch before it | `vet_completions` (no event row) |
| regain refloat | a reset for a slip class no detector sees (standing invariant 1); its re-coupling is the float vehicle's ordinary slip response, a drop to birth strength (run-end row `amb_drop_latch_slip`), not a re-seed from one epoch of code | `refloat_regain`, `refloat_regain_ambs`, `refloat_regain_reseeds`; event `band_regain ... refloat` |

A demotion window ends only after three clean epochs, so an arc that keeps flickering stays demoted while it flickers;
nothing else bounds the window. The re-entry premise counters (`reentry_unrefloated`, `lli_at_reentry` against
`lli_at_reentry_funnelled`, `reentry_amb_state_live`) read the invariant-carry funnel and the pending-reseed marks only.
The triage run reports 55 of 58 re-entries "not already refloated" and, in the same run, 69 re-entry drops latched by
the float vehicle (`amb_drop_latch_reentry`): the counters' scope on this posture is checked in section 3 (D2), not
assumed.

### 1.2 `network.raim_refloat_maturity_gate`

Site: the T4 attribution block of `networkRaim` (`src/core/network/qc/Raim.cpp`). An isolated-arc flag (a median-polish
interaction above `raim_k` times sigma0, its satellite not caught wholesale) refloats the arc: on the float vehicle a
bounded drop of each band's leveling carry (value kept, variance back to birth strength), the S-basis refusal stamp, the
AR suspect stamp and a pending reseed. With the gate on, that refloat is skipped while `networkWarmupRampLifted` is
false: until the run has emitted max(`raim_refloat_maturity_epochs`, `phase_leveling_warmup_epochs` 6) epochs and the
sigma0 EWMA has received `raim_refloat_maturity_epochs` updates (the EWMA is fed on the goldens: `emit_ura`, `raim` and
`phase_leveling` are armed). Whole-satellite refloats are not gated. Both counts only grow in a run without a resume, so
the gate has one edge per run, the lift.

| edge | what happens |
|---|---|
| suppression (each skipped flag) | an arc flagged before the lift keeps its stiff carry; a real slip that only T4 sees stays in the carry until the lift (standing invariant 1), at most N emitted epochs (1 h at N = 120) |
| lift (re-coupling) | from the first mature epoch the T4 test applies to every arc again, so a persistent misfit (a real slip) is refloated there and a warm-up transient is not; D1 in section 3 reads this edge |

The gate is silent. It logs no arming line, counts no suppressed refloat, writes no event at the lift, and the
attribution trace `network_raim_attrib.csv` marks a suppressed arc `acted=1` (triage arm `_rap_bof_tri_rmg_f2`: 24 ARC
rows `acted=1` in hour 1 and 0 `T4_attrib` refloat events in hour 1). The sibling gross-LOS screen reports its own
maturity hold at run end ("A held LOS-epoch is a COMPLETE declaration a guard refused"). Arming the key as built adds a
valve without the counters and identities of invariant 7; that code change is owed before any arming and is not made
here. The arms below read the gate through the `T4_attrib` event rows against the control.

## 2. The maturity horizon

Card BOF chose 120 epochs for the triage without a graded source. Two sources support it:

1. The gross-LOS screen's maturity horizon is the same 120 emitted epochs, measured rather than chosen
   (`gross_los_warmup_epochs`, `include/config/GnssConfigTypes.hpp`): the horizon over which the measured over-bar
   fraction settles, because a cold post-fit residual is the unconverged station block, the transient this gate
   targets.
2. The T4 isolated-arc refloats of the current field golden (`runs/_rf0hv17_field6`, 55 `T4_attrib` arc rows in 6 h)
   by half hour: 12, 14, 4, 0, then 25 over hours 3 to 6. Hour 1 runs at 26 per hour, 4.5 times the 5.8 per hour of
   hours 2 to 6; its second half hour is as high as its first, and the third half hour is at the settled rate. The
   excess spans the first hour, the 120-epoch horizon.

A second value, 60 epochs, runs as a dose arm. It suppresses the first half hour only (12 of the 26 hour-1 arc rows of
the baseline). The triage's -0.45 mm is one realization: a response at 60 between the control and 120 supports an
effect of the suppressed refloats, and a response at 60 equal to that at 120 says the first half hour carries it.

## 3. Arms and gates (frozen 2026-10-29, before any arm ran)

One exe for every arm and control (see STATUS). Every arm is a golden copy with one change, made with
`scripts/rapid_arm.py make` under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`.

| tag | base | change | length |
|---|---|---|---|
| `rcg_ctl_f6` | field golden | none | 6 h from 00:00, DOY 129 |
| `rcg_sic_f6` | field golden | `network.signal_integrity.census=true` | 6 h |
| `rcg_mg120_f6` | field golden | `network.raim_refloat_maturity_gate=true`, `network.raim_refloat_maturity_epochs=120` | 6 h |
| `rcg_mg60_f6` | field golden | the gate with `raim_refloat_maturity_epochs=60` | 6 h |
| `rcg_ctl_s6`, `rcg_sic_s6`, `rcg_mg120_s6` | sim golden | as above, each with `network.start_offset_hours=0.504167` | 6 h from 00:30:15 |

Census: `scripts/clock_level/gcensus.py make rcgc_<key> <control run> <wire run> <exe>` for each field wire (the
consumer knobs are the control's for every wire; both keys are producer keys), graded with
`gcensus.py grade rcgc_ctl rcgc_sic rcgc_mg120 rcgc_mg60`: each site's mean hourly p50 over hours 2 to 6, paired by
site against the control wire.

Gates, per key, arm against the same-exe control:

| gate | measure | bar |
|---|---|---|
| G1 | held-out consumer WTZZ00DEU (in-run, `logs/closed_loop_trace.csv`, 3D p50 by hour, hour 0 at the first row, `scripts/clock_level/p50.py`) | mean change over hours 2 to 6 at most +1.0 mm, and no hour of 2 to 6 worse by more than 1 cm |
| G2 | 16-site census (`gcensus.py`) | paired median change at most +1.0 mm, and no site worse by more than 1 cm |
| G3 | withholds, run end ("Network producer complete") | the arm emits as many epochs as the control and withholds none the control does not |
| G4 | refloat churn: the slip-class ambiguity responses of the run-end response table, `amb_raim_drop` + `amb_drop_latch_slip` + `amb_gross_erase` (arcs whose prior a T4 or T3 drop, a slip-funnel drop or a gross-LOS erase changed) | at least 5 % below the control (a smaller change reads "churn unchanged") |
| G5 | sim, for a key that acts on the sim: SZZ000SIM 3D p50 by hour from the run start (00:30:15); withholds as G3 | mean change over hours 2 to 6 at most +1.0 mm, no hour worse by more than 1 cm, no added withhold |

A key acts on the sim when its sim arm's `products.txt` or `logs/closed_loop_trace.csv` differs from the sim control's;
a byte-identical sim arm records G5 as not applicable (the sim has no IF-TD candidate and, in the triage, no T4 refloat).

Verdict, per key:

- **PASS:** G1 to G5 hold (G5 where it applies). The key moves to the "less churn" section of the register with its
  numbers.
- **HARMLESS, NOT LESS CHURN:** G1, G2, G3 and G5 hold and G4 misses. The key moves to the "partial signals" section with
  its numbers.
- **FAIL:** G1, G2, G3 or G5 misses. The key moves to the "partial signals" section with the failed clause.

The maturity gate's verdict is read at 120 epochs, the triage value; the 60-epoch arm is graded on the same gates and
reported as evidence on the value. A census fail carried by ARHT00ATA alone (the site that decided every "no site worse"
clause of card BOF) is reported as such, beside any measurement of that site's spread under a neutral change available
at grading time; the verdict on the written bar stands.

Reported, no bar:

- **D0** the control against `runs/_rf0hv17_field6` (`products.txt`, `logs/closed_loop_trace.csv`): expected identical,
  card CVF's products being byte-identical. A difference is reported and does not change the same-exe gates.
- **D1** (maturity gate, the lift edge) `T4_attrib` arc rows by half hour, arm against control; the first mature epoch;
  the arc rows in the ten emitted epochs after it, against the control's in the same epochs. A burst at the lift means
  hour-1 flags were persistent misfits; none means they were transients that resolved.
- **D2** (census, its edges) demoted line-of-sight epochs and rows, vet completions, regain refloats and their two
  feeds, re-entries and the premise counters, LLI at re-entry, re-entries with live state; and the share of the census's
  `reentry` events with an ambiguity drop recorded at the same epoch in `logs/network_amb_state.csv`, which tests the
  premise counters' scope (section 1.1).
- **D3** hour 1 (ignition) for every arm, all ambiguity rows of the run-end response table, and wall time.

## 4. Results (2026-10-30)

### 4.1 Runs

The seven arms ran at once on the exe of the STATUS block (wall 1890 to 1912 s field, 1106 to 1121 s sim, on a shared
box). Every arm exits 0 and emits 720 epochs with none withheld. D0: the control is byte-identical to
`runs/_rf0hv17_field6` in `products.txt`, `logs/closed_loop_trace.csv`, `logs/network_solution.csv` and
`logs/network_epoch_status.csv`, so the arms grade against the baseline pair of card BOF. The 68 census replays exit 0.
Every number below was recomputed from the run directories by a second script; the consumer numbers agree with
`scripts/clock_level/p50.py` and the census numbers with `gcensus.py grade`.

### 4.2 Gates

| gate | bar | census (`sic`) | gate at 120 (`mg120`) | gate at 60 (`mg60`) |
|---|---|---|---|---|
| G1 WTZZ00DEU, mean change over hours 2 to 6 (worst hour) | +1.0 mm (+10 mm) | +0.10 mm (+0.55, hour 3): pass | +0.01 mm (+0.10): pass | +0.01 mm (+0.05): pass |
| G2 census, paired median (sites worse) | +1.0 mm (none) | +0.05 mm (0): pass | +0.06 mm (0): pass | +0.04 mm (0): pass |
| G3 epochs emitted / withheld | as the control, 720 / 0 | 720 / 0: pass | 720 / 0: pass | 720 / 0: pass |
| G4 slip-class responses (control 577) | -5 % or lower | 558, -3.3 %: miss | 537, -6.9 %: pass | 559, -3.1 %: miss |
| G5 sim SZZ000SIM, mean change over hours 2 to 6 (worst hour) | +1.0 mm (+10 mm) | +0.02 mm (+0.10): pass | byte-identical: not applicable | not run |

Held-out consumer 3D p50 by hour, hours 1 to 6 (mm):

| run | p50 |
|---|---|
| field control, WTZZ00DEU | 58.80 12.30 23.05 12.25 16.50 12.00 |
| census | 58.80 12.10 23.60 12.60 16.60 11.70 |
| gate at 120 | 58.65 12.30 23.15 12.25 16.50 11.95 |
| gate at 60 | 58.65 12.35 23.00 12.25 16.55 12.00 |
| sim control, SZZ000SIM from 00:30:15 | 99.45 32.60 39.80 16.00 18.00 15.15 |
| sim census | 99.45 32.60 39.80 16.10 18.05 15.10 |

Run-end ambiguity responses (the G4 rows):

| row | control | census | gate at 120 | gate at 60 |
|---|---:|---:|---:|---:|
| `amb_raim_drop` (T4 and T3) | 107 | 78 | 55 | 83 |
| `amb_drop_latch_slip` | 456 | 466 | 468 | 462 |
| `amb_gross_erase` | 14 | 14 | 14 | 14 |
| sum | 577 | 558 | 537 | 559 |

The other ambiguity rows (re-entry drop 222, stale-gap drop 2008, gap prune 2011, anchor re-reference erase 216, the
two free-epoch rows 0) are identical in the four field arms. Every T4 drop not taken is partly taken later: the slip
drop counts only an arc whose prior it changed, and an arc a T4 drop had left at birth strength did not count before
(`amb_drop_latch_slip` +6 to +12).

Census detail: in hours 2 to 6 no site moves by more than 1.2 mm. The largest changes are ARHT00ATA's (+0.48, +1.01,
+1.18 mm for the census, 120 and 60) and DAV100ATA's (+0.58, +0.40, +0.22 mm). Sign counts over the 16 sites: 6 lower and
10 higher (census), 4, 11 and 1 equal (120), 3, 11 and 2 equal (60). Hour 1 paired median -0.00, +0.30, +0.38 mm.

### 4.3 The maturity gate (D1)

- T4 arc rows by half hour: control 12 14 4 0 3 8 0 4 0 2 2 6 (55); gate at 120: 0 0 4 0 3 8 0 4 0 2 2 6 (29); gate at
  60: 0 14 4 0 3 8 0 4 0 2 2 6 (43).
- Suppressed flags (attribution-trace ARC rows `acted=1` with no refloat): 27 at 120, the last at 60.0 min; 12 at 60,
  all in the first 14 min.
- The lift: the first refloat comes at 64.0 min (120) and 33.0 min (60), consistent with N emitted epochs plus the
  6-epoch carry warmup before the sigma0 EWMA, which the RAIM seam feeds, starts counting (the service-health trace
  reads 126 emitted epochs at 63.0 min).
- After the last suppressed flag, each arm's T4 refloats equal the control's row for row (29 of 29 at 120, 43 of 43 at
  60): the lift adds no catch-up. At 120, three arcs flagged before the lift are flagged again at 64.0 min in both runs:
  HKSL00HKG E04, E13 and E19, which the control refloats two to four times between 51 and 59 min and once more at
  64.0 min, so its early refloats did not clear the misfit; the arm refloats them once, at 64.0 min. Every other
  suppressed flag left no trace: a warm-up transient that resolved by itself, the case the gate is designed for (on
  this day).
- Dose: T4 refloats 107, 83 and 55 at 0, 60 and 120 epochs. The users do not move at either value.

### 4.4 The census (D2)

- Field: 1608 demoted line-of-sight epochs (2302 phase rows at sigma x 10), a further 7659 exempt as never converged,
  2189 vet completions; 76 flicker-regain refloats (0 funnel insertions, 132 pending reseeds); SIGNAL events: 238
  re-entries, 112 band losses, 76 regains.
- The T4 refloats the census removes are 15 rows of the first two hours (12 in hour 1, 3 in hour 2: HKSL00HKG 8,
  MAC100AUS 4, YKRO00CIV 3); it adds one in hour 6 (JPLM00USA / G10). The other 40 rows are the control's.
- Sim: 574 demoted line-of-sight epochs (1148 phase rows) at 144 re-entries; no band loss or flicker, so no regain
  refloat.
- The premise counters report 198 of 238 re-entries "NOT already refloated" and 63 of 64 loss-of-lock assertions on a
  re-entry epoch "did NOT reach a refloat funnel". `logs/network_amb_state.csv` records at the same epoch, for the same
  station and satellite:

| state at the re-entry epoch | re-entries |
|---|---:|
| a `relevel_free` drop (4 with a T4 drop as well, 1 beside a birth) | 72 |
| a fresh birth only | 82 |
| a birth and an anchor re-reference erase | 11 |
| a birth on one band and a carried level on another | 7 |
| a carried level, no event | 22 |
| an anchor re-reference erase | 1 |
| no carried row | 43 |

The counters read the invariant-carry funnel (off on the goldens) and the pending-reseed marks only, so on this posture
they count the float vehicle's re-entry drops as "not refloated". The hole they measure is at most the 29 re-entries
that brought a carried level across the gap with no response at the return epoch; the gap re-entry response leaves
such a level alone unless the raw MW or GF level shifted across the break or LLI was set. Not explained here: the
census's own warning of 25 re-entries "with a SURVIVING ambiguity state", which it calls structurally impossible.

### 4.5 Verdicts

- **`network.raim_refloat_maturity_gate` at 120 epochs: PASS** (G1 to G4; G5 not applicable). It moves to the "less
  churn" section of the register. Owed before any arming, a code change not made here: a counter of the suppressed
  refloats, an event at the lift, and an `acted` column that reflects the gate (section 1.2).
- **`network.signal_integrity.census`: HARMLESS, NOT LESS CHURN** (G1, G2, G3 and G5 pass; G4 misses at -3.3 %). It moves
  to the "partial signals" section. Its purpose, keeping an unmonitored phase off full weight across a flicker (the
  design case is at 19:22 to 19:26 UT, outside the goldens' window), is not what these gates measure. Its premise
  counters need the float vehicle's re-entry drop before their warnings mean what they say.
- **60 epochs:** G1 to G3 pass and G4 misses (-3.1 %): the shorter horizon keeps out the first half hour's transients and
  lets the second half hour's 14 through.
- Neither key moves a user. The triage's hour-2 gains (-0.40 and -0.45 mm on the golden of `ef9f75d`) do not reproduce
  on the current golden (hour 2: -0.20 mm for the census, 0.00 mm for the gate at 120).
