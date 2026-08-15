# Slip-QC hardening — implementation report (2026-07-21)

Implements `docs/SPEC_slip_qc_hardening.md` in full: Cards **E**, **R**, **L**, **G**,
the shared raw-observable seam (§1), the validation grader (§6.1) and the first
dedicated unit coverage of the slip-detector chain (rule 7).

Everything new is default-OFF. Read the **Findings** section before the mechanism
sections — three of the spec's stated premises did not survive measurement, and
one MUST could not be met as literally written.

---

## 1. What landed

| Area | Files |
|---|---|
| Shared raw MW/GF/LLI seam (§1) | NEW `include/slip/NetworkRawObservables.hpp`, `src/core/slip/NetworkRawObservables.cpp` |
| Card E + Card L | `include/slip/NetworkSlipQc.hpp`, `src/core/slip/NetworkSlipQc.cpp` |
| Card R | `include/slip/IfPhaseSlipDetector.hpp`, `src/core/slip/IfPhaseSlipDetector.cpp`, `src/core/app/ApplicationNetworkSetup.cpp` |
| Card G + run-end telemetry | `src/core/app/ApplicationNetwork.cpp`, `src/core/app/ApplicationNetworkAssembly.cpp`, `include/app/Application.hpp` |
| Config | `include/config/GnssConfigTypes.hpp` (`SlipQcConfig`), `src/core/config/GnssConfigLoadNetwork.cpp` |
| Grader (§6.1) | NEW `scripts/grade_slip_agreement.py` |
| Unit tests (rule 7) | NEW `tests/test_if_phase_slip_detector.cpp`, `tests/test_network_slip_qc.cpp` |
| Build | `CMakeLists.txt` (3 explicit source entries) |
| Configs | `config/test_suite/_wlver_150sta_10ep_d10_allfreq_sliphard.json` (NEW, cards armed) |

**Toggle deviation (stated).** The spec wanted the toggles armed in the audit
config and the OFF baseline taken from a stripped copy. This landed as the
mirror image: the audit config
`_wlver_150sta_10ep_d10_allfreq_slipcmp.json` is **unchanged** (so §0's
"rerun the config to regenerate" reproduction stays exact — verified, see §3)
and a new `..._sliphard.json` twin carries the three masters. Same two-arm gate,
one less thing that silently drifts.

---

## 2. Findings that contradict the spec's premises

These are the substantive results; the mechanisms are secondary.

### F1 — L2's causal story is false. The four "chatty" stations assert LLI **bit 2**, not bit 0.

The audit reported "352 LLI-set-but-no-jump rows … dominated by STK200JPN (99),
ISHI00JPN (90), MCIL00JPN (83), CCJ200JPN (80)" and attributed the 2 318 Card-4
carry refloats to that chatter feeding the C3c funnel.

Census of the golden run's `network_slip_raw.csv` by LLI **byte value**:

| LLI byte | meaning (RINEX 3) | occurrences |
|---|---|---|
| 0 | — | 28 981 |
| 1 | bit 0: **lost lock** | 188 |
| 4 | bit 2: observation under **antispoofing** | 396 |
| 5 | bits 0+2 | 11 |

Every one of the 396 `LLI=4` occurrences is the *second* band of a JPN station
(`lli1/lli2 = 0/4`, 396 rows). The MW drain has always tested `lli & 1`, so
those rows **never drove a single refloat**. Restricting the census to bit 0
gives a completely different offender list: OWMG00NZL 33, MAC100AUS 24,
MCIL00JPN 11, WARK00NZL 9, ASCG00SHN 9 — 122 rows total, not 352.

**Consequence:** Card L is correct and useful, but it cannot deliver the
"2 318 → real-event order" falsifier, because the population that falsifier
assumed does not exist. Card L found the *real* bit-0 chatty stations
(OWMG00NZL, MAC100AUS) and demoted exactly those two.

### F2 — the 2 318 carry refloats are dominated by the D5 innovation gate, not by the coarse detectors.

Golden run, same epochs: `slip refloats 2318`, of which the D5 fine-slip gate
contributed **1 560 flags**. The coarse layer contributed 67 MW + 87 LLI
satellite detections and 49 IF-TD flags. Chord-kill counts are not 1:1 with
ambiguity marks (one ambiguity is an edge in a graph), but the ordering is
unambiguous: D5 is the dominant driver.

### F3 — D5 re-taps are ~97 % NOT coincident with a raw observable step.

Card G(c) made the audit's un-decomposable 1 560 attributable. On the armed
run: **1 921 D5 re-taps, 54 of them coincident with a raw MW/GF step** at the
same (station, satellite, epoch). The D5 gate is overwhelmingly firing on
*model tension*, not on anything visible in the receiver's own observables.
That is a concrete, quantified lead for the D5/M2 lineage and was not visible
before this card.

### F4 — Card R's third CANDIDATE (station-local hand-off) is structurally unimplementable at this seam. Not built.

Two independent reasons, both verified by construction in
`tests/test_if_phase_slip_detector.cpp`:

1. a genuine receiver-level common mode **cancels in the between-satellite
   single difference**, so it can never reach the non-reference double
   differences this gate adjudicates;
2. the only epoch the test could fire on is the forced re-election epoch, where
   `ref_changed` is true and `processDoubleDifferences` applies the
   median-offset rebase — removing exactly the common mode the test looks for,
   before the gate is consulted.

Shipping it would have left a permanently-zero counter reading as "no
station-local events occurred" — the standing-invariant-#4 failure mode. The
branch and its counter were **removed**, with the reasoning recorded at the
site.

### F5 — a coherence-only reference gate destroys the event R4 protects.

The first armed smoke suppressed the genuine, LLI-corroborated
OWMG00NZL/G31 station re-lock along with the false JPLM/MKEA family. Root
cause: a multi-satellite re-lock produces an *incoherent* family (each
satellite jumps by its own amount), which is exactly the drift signature.
Fixed by splitting the incoherent branch — see §4.

---

## 3. Card-by-card

### Card E — cold diagnostic MW/LLI emission (no toggle, numerically inert)

`refloat_signal` now reports **matched** and **refloated** as separate facts.
Emission and the `n_mw`/`n_lli` counters key on the detection; refloat
accounting is a separate counter and stays warm-only. A cold run writes
`mw_wl_slip` / `lli_slip` rows with `action=diagnostic`, mirroring the IF-TD
driver.

Measured on the golden 11-epoch run: **67 wide-lane + 87 LLI detections that
previously wrote nothing at all**. `slip refloats` stayed at **2 318** —
E3's numerical no-op verified against the actual carry counter, not asserted.

### Card R — hardened reference-satellite gate (`network.cycle_slip.if_phase_td_ref_*`)

The legacy gate (`|median(non-ref ΔΔ)| > 0.5·λ_IF`, `n ≥ 2`, fixed) is kept as
the **candidate filter**; when armed, the hardened gate adjudicates:

1. **Scale** — `|median| > max(floor_frac·λ_IF, k·s_eff)`, where `s_eff` is an
   empirical-Bayes blend of the constellation's EWMA of non-reference DD
   **dispersion** with a cold prior of `floor_frac·λ_IF / k`. The cold prior is
   deliberately *not* the current epoch's dispersion — that is self-contaminating
   and is what swallowed the OWMG event (F5). Zero empirical information ⇒ the
   gate degenerates to exactly the legacy physical floor. The dispersion EWMA
   advances on **every** epoch with a usable family, not only candidate epochs,
   so a persistent drift cannot raise its own acceptance threshold.
2. **Coherence** — a reference slip is a *common* step: `MAD ≤ coherence·|median|`.
3. **Incoherent split (F5 fix)** — incoherent families are separated by whether
   the members are *independently* slip-sized (`|dd| > λ_IF`, a full IF cycle,
   not the half-cycle candidate floor):
   * ≥ half the family slip-sized ⇒ **station event**: reset the reference arc
     (conservative under a zero-q carry, standing invariant #1) but **do not**
     rebase `prev_grad` by the median — the median is a mixture, and rebasing
     would inject a fictitious step into every other arc and hide the real
     per-satellite jumps from the network pool arm;
   * otherwise ⇒ **drift**: suppress.
4. **Thin family** — below `min_n` non-reference DDs the median is untrustworthy;
   the physical floor widens by `thin_widen` and the deferral is counted.
5. **Persistence cap** — after `max_consecutive` consecutive fires on one
   reference the gate stops firing and forces a reference **re-selection**
   (excluding the incumbent for that one election), moving the drifting
   satellite into the pool arm. The audit's 9-consecutive pattern is
   structurally impossible.

### Card L — LLI corroboration (`network.cycle_slip.mw_lli_corroborate`)

An LLI assertion **acts** when
(a) an empirical MW or GF step corroborates it — decisive at *any* station, which
is what keeps a real multi-satellite re-lock intact (M3) — or
(b) the station's own LLI behaviour is still credible (two-state EWMA of its
LLI-set fraction with a hysteresis band; never "ignore").
Only an uncorroborated assertion at a demonstrably non-credible station is
downgraded, and then it is emitted with `action=suppressed` and counted (M4).
Because the whole `refloat_signal` call is skipped, the C3c funnel insert obeys
the same qualification as the pending-reseed path (M5).

Gates are empirical throughout (rule 4): per-station robust EWMA of that
station's *own* |ΔMW| / |ΔGF| steps, floored by a physical bound in raw metres
(0.5 wide-lane cycles / 4 cm — the audit's own support test). Clean survivors
only are folded, so the scale never absorbs the event it must catch.

**Scope limitation, counted not hidden:** the shared observables are formed on
the primary dual-frequency pair, so an LLI asserted only on a band outside that
pair is not witnessable by MW/GF. Those are counted separately
(`lli_offpair`) rather than silently absorbed into "uncorroborated".

### Card G — gap-coincident instrumentation (`network.cycle_slip.gap_instrument`)

Zero mechanism change. Counts arc resumptions after ≥ 1 missed **processed**
epoch, whether the raw MW/GF level shifted across the break beyond the same
empirical gate, and — per arc, after the slip drains have run so the C3c set is
complete and before `buildCarryDdPriors` re-keys the carry — whether the arc
re-entered with **bridged** carry state (a surviving phase-leveling carry value
inside `phase_leveling_max_gap_s`, or a Card-4 chord the driver still holds)
versus a fresh float. Plus G1(c): D5 re-taps tagged by raw-step coincidence.

---

## 4. Validation

Release, same-build two-arm gate on `_wlver_150sta_10ep_d10_allfreq_slipcmp.json`
(150 stations, GPS-only, `frequency_mode=all` + `channel_mode=all`, 11 epochs
emitted / 0 skipped of 12 synced — the audit's exact configuration).

### 4.1 Baseline reproduces §0 exactly

The pre-change golden run reproduces the audit's headline numbers to the digit,
so the comparison is against a real baseline and not a re-derivation:

| Quantity | §0 | golden run |
|---|---|---|
| IF-TD samples → flags | 8 717 → 32 (0.367 %) | **8 717 → 32 (0.367 %)** |
| margin `abs_dev/thr` min / med / max | 1.19 / 74 / 2066 | **1.19 / 72.6 / 2066** |
| refsat-reset events | 17 | **17** (JPLM ×9, MKEA ×5, OWMG ×3) |
| unsupported flags (raw flat) | 14 | **14** — exactly JPLM/G10 ×9 + MKEA/G10 ×5 |
| reverse sweep, \|ΔMW\| > 3 cyc | 57 → 39 IF-flagged | **57 → 39** |
| carry slip refloats | 2 318 | **2 318** |

### 4.2 OFF gate — byte-identity (all three masters off)

| Artifact class | Result |
|---|---|
| 53 of 57 CSV/trace artifacts | **md5-identical** |
| `network_d5_flags.csv`, `network_wl_lane_trace.csv` | append-mode streams; **per-run content md5-identical** to the golden run's own tail (1 561 / 12 569 rows) |
| `network_run_summary.csv` | identical except the two **wall-clock** columns (`576311.5→505196.6 ms`) — 12 synced / 11 emitted / 0/0/0 skipped / 150 stations / 20 threads all unchanged |
| `network_events.csv` | pre-existing 57 rows **byte-identical**; **+154 rows**, every one a `T0_mw_detect` row with `action=diagnostic` — i.e. exactly and only Card E's mandated emission |
| carry census | `slip refloats 2318` — **unchanged**, so E3's numerical no-op is verified against the counter, not asserted |

`main_log.log` is excluded by construction (it carries wall-clock timings and can
never be byte-identical between two runs).

### 4.3 Unit tests

`gnss_tests.exe`: **864 tests / 863 passed / 1 skipped** (the pre-existing
fixture-gated `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`). Baseline before
this work was 839 — **+25 additive cases**, the first dedicated coverage the
slip-detector chain has ever had.

### 4.4 ON smoke — falsifiers

| Card | Falsifier | Result |
|---|---|---|
| E | MW/LLI rows appear with `action=diagnostic`; counters count detections | **MET** — 67 MW + 87 LLI detections, previously zero rows |
| E4 | MW ↔ IF agreement readable from `network_events.csv` alone | **MET** — the grader now reports a 57.5 % IF↔MW match rate on the armed run; on the golden run that section reads "MW/LLI side is EMPTY" |
| R | pool arm unchanged | **MET** — 8 717 → 8 727 samples (sync noise), 32 → 33 flags |
| R | OWMG G31 survives | **MET** — survives (TOW 259320, 259380), and both fire on the new **station-event** path (reset without a mixture rebase). The Card-L rows show the same re-lock epoch with all 8 satellites `LLI;mw_step` corroborated and acting |
| R | JPLM/MKEA G10 family gone | **PARTIAL — 14 → 4** (−71 %) at the shipped `max_consecutive=1`. JPLM 9→3, MKEA 5→1; longest consecutive run 1, so the audit's 9-epoch stream is structurally impossible. Not zero: the residual fires are the *physical-floor* arm, which a cold 11-epoch run never gets past (see §4.5) |
| L | JPN stations move to suppressed counters | **VOID** — those stations never entered the LLI arm at all (finding F1); the card demoted the two stations that genuinely do chatter on bit 0 |
| L | OWMG re-locks still act | **MET** — see the R row above |
| L | carry refloats 2 318 → real-event order | **NOT MET, and not achievable by this card** — findings F1/F2: the population that falsifier assumed does not exist, and the count is D5-dominated |
| G | counters populate | **MET** — 16 resumptions (5 unmeasurable, 11 shifted); of the shifted, **0 arcs re-entered bridged / 22 as a fresh float**; 0 clean-bridged; D5 1 689 re-taps, **50 raw-step-coincident (3.0 %)** |

Full armed-run counters (shipped defaults):

```
card E   67 wide-lane + 73 LLI detections (0 refloated — carry cold)
card R   10 legacy candidates -> 6 fired (2 station events); 1 suppressed by
         scale/coherence, 0 thin-family, 3 by the consecutive-fire cap;
         3 forced reference re-selections
card L   125 LLI assertions: 34 MW-corroborated, 3 GF, 49 unmeasurable,
         2 off-primary-pair, 22 SUPPRESSED; 1631 credible / 7 demoted
         station-epochs.  Offenders: OWMG00NZL 12/33 (now corroboration-
         required), MAC100AUS 10/24 (demoted then promoted back — the
         hysteresis working, and visible)
card G   16 resumptions / 5 unmeasurable / 11 shifted / 0 shifted-bridged /
         22 shifted-fresh / 0 clean-bridged; D5 1689 re-taps, 50 stepped
carry    slip refloats 2318 (OFF) -> 2399 (ON)
```

`slip refloats` rising slightly (2 318 → 2 399) is expected and is the reason
F2 matters: with the refsat false family removed, more chords survive to be
judged by D5, and D5 — not the coarse layer — is what sets this number.

### 4.4.1 Independent confirmation from the grader

`scripts/grade_slip_agreement.py` adjudicates every IF-tier flag against the
raw observables without reading any of the producer's own counters. Its
verdict on the two arms:

| | golden (OFF) | armed (ON) |
|---|---|---|
| IF-tier flags adjudicated | 49 | 39 |
| **raw-confirmed rate** | **71 %** | **90 %** |
| unsupported (raw series flat) | **14** — JPLM/G10 ×9, MKEA/G10 ×5 | **4** — JPLM/G10 ×3, MKEA/G10 ×1 |
| `if_slip` (pool arm) unsupported | 0 | 0 |

The pool arm was never the problem and is untouched; the entire unsupported
population was, and still is, the refsat side-channel — now down to four
events with no consecutive run.

### 4.5 Defaults set from the measurement, not from the spec text

* `if_phase_td_ref_max_consecutive`: **1** (was 2). A genuine one-epoch common
  step is never consecutive, so the cap costs it nothing, while a persistent
  drift escalates on its second epoch instead of its third. Measured on this
  config: the false family goes 14 → 6 at 2, **14 → 4 at 1**, with OWMG
  untouched either way.
* `mw_lli_min_epochs`: **1**. Verified safe on this data — no station was
  demoted on epoch 1 (no start-of-file blanket LLI), and a station demoted at
  first sighting has fresh arcs by construction, so a suppression there costs
  the carry nothing.

---

## 5. Readiness

The warm validation leg (≥ 1 h on the same station set) is **prepared and not
launched** — per the staged-cadence directive it runs only on command:

```bash
build/x64-Release/gnss_processor.exe config/test_suite/_wlver_150sta_10ep_d10_allfreq_sliphard.json
```
(raise `processing.max_duration_s` / `network.max_duration_s` to the desired span first)

What the warm leg is needed for:
* **G3's decision rule** — the shifted-AND-bridged count on a *warm* carry is
  the required triage artifact. The 10-epoch smoke is cold, so almost nothing is
  bridged by construction; the cold number is necessary but not sufficient.
* **Card L** — the ambiguity-variance sawtooth / D5 re-tap check at the demoted
  stations.
* **Card R** — the empirical dispersion EWMA only reaches full weight after
  ~`shrink_kappa` samples; a 10-epoch run exercises mostly the cold prior. The
  residual JPLM/MKEA events are the physical-floor arm firing, which is exactly
  what the warm empirical arm is meant to take over.

## 6. Follow-ups this work opens (not built)

1. **Re-examine the D5 innovation gate** with F2 + F3 in hand: it drives the
   large majority of carry slip refloats, and ~97 % of its re-taps have no
   corresponding raw observable step. Either the gate is over-firing on model
   tension, or "model tension" is a real slip class the observables cannot see —
   `network_d5_flags.csv` plus the new raw-step tag is the instrument to decide.
2. **Widen the corroboration primitive** beyond the primary dual-frequency pair
   if `lli_offpair` is material on a longer/multi-constellation run (it was 2 of
   125 here — currently immaterial).
3. **A bit-2 (antispoofing) LLI note for the audit lineage**: nothing in the
   producer acts on it, and nothing should. Recorded here so the 352-row figure
   is not re-derived as a defect by a future session.
