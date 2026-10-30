# RCG: the two keys that cut refloats in card BOF's triage, graded on 6 h arms

**STATUS**

- **id:** RCG
- **topic:** raim-qc
- **status:** open
- **summary:** The two never-graded keys that moved the solve in card BOF's 2 h field triage (BOF section 3), graded as BOF section 5 asks: 6 h field arms and the 16-site census against a same-exe control on the current golden, and a 6 h sim arm for a key that acts on the sim. `network.signal_integrity.census` (triage: T4 refloats 60 to 30, held-out consumer hour 2 -0.40 mm) and `network.raim_refloat_maturity_gate` with `raim_refloat_maturity_epochs` 120 (triage: T4 refloats 60 to 8, hour 2 -0.45 mm), plus a second horizon of 60 epochs. Gates in section 3, written and committed before any arm ran. Nothing is armed here: the author decides.
- **source:** [BOF](BOF_built_off_follow_through_2026-10-28.md) sections 3 and 5; worker report `D:/GnssProcessorSupport/runs_nom/bof_reports/triage_2h.md`; the "never graded" section of [OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md); ledger rows of the keys in `docs/CONFIG_promotion_ledger.json`
- **knobs / posture:** read, not changed: `network.signal_integrity.census` (UNVALIDATED; its PARAM children at their loader defaults: `unvetted_sigma_scale` 10, `revet_ep` 3, `flicker_max_ep` 5, `refloat_on_regain` true, `withhold_instead` false), `network.raim_refloat_maturity_gate` (UNVALIDATED) with `network.raim_refloat_maturity_epochs` (PARAM) 120 and 60. Bases: `config/golden/golden_field_refound.json` and `config/golden/golden_sim_refound.json` of main `1527cf9`.
- **dates in the text:** 2026-10-29
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/worktrees/mgsi/build/x64-Release/gnss_processor_rcg.exe` (the Release build of main `1527cf9` in a clean detached worktree, sha256[:16] fab88a924e9189fd); run root `D:/GnssProcessorSupport/runs_nom`: field arms `_rap_rcg_{ctl,sic,mg120,mg60}_f6`, sim arms `_rap_rcg_{ctl,sic,mg120}_s6`, census replays `_rap_rcgc_{ctl,sic,mg120,mg60}_<site4>`
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: do the two keys that cut refloats on one 2 h arm keep the held-out consumer, the 16-site census
and the sim within their bars over 6 h, and do they cut the refloat churn? Pre-registered 2026-10-29; results pending.

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
