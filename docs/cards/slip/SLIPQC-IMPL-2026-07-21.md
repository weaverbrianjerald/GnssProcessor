# slip-QC impl: build record of cards E, R, L and G (2026-07-21)

**STATUS**

- **id:** slip-QC impl
- **topic:** slip
- **status:** closed
- **summary:** Cards E, R, L and G LANDED default OFF; Card R PARTIAL (unsupported refsat flags 14 to 4), two of Card L's three falsifiers VOID or NOT MET; five findings contradicted the spec's premises.
- **dates in the text:** 2026-07-21 to 2026-08-12
- **knobs / posture:** `network.cycle_slip.mw_lli_corroborate`, `network.cycle_slip.gap_instrument`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_slip_qc_hardening.md`, sections 8, 8.1, 8.2, 8.3, 8.4

**Outcome.** The card records the build of the slip-QC hardening spec ([slip-QC](../slip/SLIPQC-REF.md)) and its same-build two-arm gate on a cold 11-epoch, 150-station run. OFF was byte-identical, Card E MET, and the grader's raw-confirmed rate rose from 71 % to 90 %. F1 (the chatter is LLI bit 2, never acted on) and F2 (the D5 gate dominates the 2 318 carry refloats) left Card L without its assumed population; F3 found about 97 % of D5 re-taps with no raw step; F4 left Card R's station-local hand-off unbuilt. The warm leg (Card W) was never launched; Card R's warm takeover was validated later ([IF-TD ref gate](../slip/IFTD-REF-GATE.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
## 8. Implementation record, 2026-07-21 (absorbed `slip_qc_hardening_report_2026-07-21.md`)

All default-OFF. **Read §8.2 before §8.3** — three premises did not survive measurement and one MUST
could not be met as literally written.

### 8.1 What landed

| Area | Files |
|---|---|
| Shared raw MW/GF/LLI seam (§1) | NEW `include/slip/NetworkRawObservables.hpp`, `src/core/slip/NetworkRawObservables.cpp` |
| Card E + Card L | `include/slip/NetworkSlipQc.hpp`, `src/core/slip/NetworkSlipQc.cpp` |
| Card R | `include/slip/IfPhaseSlipDetector.hpp`, `src/core/slip/IfPhaseSlipDetector.cpp`, `src/core/app/ApplicationNetworkSetup.cpp` |
| Card G + run-end telemetry | `src/core/app/ApplicationNetwork.cpp`, `src/core/app/ApplicationNetworkAssembly.cpp`, `include/app/Application.hpp` |
| Config | `include/config/GnssConfigTypes.hpp` (`SlipQcConfig`, `:370` / `:911`), `src/core/config/GnssConfigLoadNetwork.cpp:3191` |
| Grader (§6.1) | NEW `scripts/grade_slip_agreement.py` |
| Unit tests (rule 7) | NEW `tests/test_if_phase_slip_detector.cpp`, `tests/test_network_slip_qc.cpp` |
| Build / configs | `CMakeLists.txt` (3 explicit source entries); NEW `config/test_suite/_wlver_150sta_10ep_d10_allfreq_sliphard.json` (cards armed) |

**Toggle deviation (stated).** The spec wanted the toggles armed in the audit config with the OFF
baseline from a stripped copy; it landed as the mirror image — the audit config
`_wlver_150sta_10ep_d10_allfreq_slipcmp.json` is **unchanged** (so §0's reproduction stays exact,
§8.4.1) and a new `..._sliphard.json` twin carries the three masters. Same two-arm gate, one less
thing that silently drifts.

### 8.2 Findings F1–F5 that contradict the spec's premises

| finding | content |
|---|---|
| **F1** | **L2's causal story is false: the four "chatty" stations assert LLI bit 2, not bit 0.** Census of the golden run's `network_slip_raw.csv` by LLI byte: `0` → 28 981; `1` (bit 0, **lost lock**) → 188; `4` (bit 2, **antispoofing**) → 396; `5` (bits 0+2) → 11. **All 396 `LLI=4` rows are the SECOND band of a JPN station** (`lli1/lli2 = 0/4`). The drain has always tested `lli & 1`, so they **never drove a single refloat**; bit-0 census is **122 rows, not 352**. Card L is correct and useful but cannot deliver the "2 318 → real-event order" falsifier — the assumed population does not exist. It found the *real* bit-0 chatterers and demoted them. |
| **F2** | **the 2 318 carry refloats are dominated by the D5 innovation gate, not the coarse detectors.** Golden run, same epochs: `slip refloats 2318`, of which D5 contributed **1 560 flags**; the coarse layer contributed **67 MW + 87 LLI** satellite detections and **49 IF-TD** flags. Chord kills are not 1:1 with ambiguity marks (one ambiguity is a graph edge), but the ordering is unambiguous: **D5 dominates.** |
| **F3** | **D5 re-taps are ~97 % NOT coincident with a raw observable step.** Card G(c) made the audit's un-decomposable 1 560 attributable: on the armed run **1 921 D5 re-taps, 54 coincident with a raw MW/GF step** at the same (station, satellite, epoch) — the gate fires on **model tension**, not on anything visible in the receiver's own observables. *(Cited by `include/config/GnssConfigTypes.hpp:2622`, `rep_error_d5_keep`. **ANSWERED from a different direction — §11.**)* |
| **F4** | **Card R's third CANDIDATE (station-local hand-off) is structurally unimplementable at this seam. NOT BUILT.** Two independent reasons, both verified by construction in `tests/test_if_phase_slip_detector.cpp`: (1) a genuine receiver-level common mode **cancels in the between-satellite single difference**, so it can never reach the non-reference double differences this gate adjudicates; (2) the only epoch it could fire on is the forced re-election epoch, where `ref_changed` is true and `processDoubleDifferences` applies the median-offset rebase (`IfPhaseSlipDetector.cpp:546-557`) — removing exactly the common mode the test looks for, before the gate is consulted. Shipping it would have left a permanently-zero counter reading as "no station-local events occurred" — the invariant-#4 failure mode. **The branch AND its counter were REMOVED**, reasoning at `IfPhaseSlipDetector.cpp:485-495`. |
| **F5** | **a coherence-only reference gate destroys the event R4 protects.** The first armed smoke suppressed the genuine, LLI-corroborated OWMG00NZL/G31 station re-lock along with the false JPLM/MKEA family: a multi-satellite re-lock produces an *incoherent* family (each satellite jumps by its own amount), exactly the drift signature. Fixed by the incoherent split (§8.3). **Standing rule: the empirical-Bayes cold prior is `floor_frac·λ_IF / k` and NEVER this epoch's dispersion** — the latter is self-contaminating and is what swallowed OWMG. |


### 8.3 Card-by-card as built

**Card E (no toggle, numerically inert).** `refloat_signal` reports **matched** and **refloated** as
separate facts; emission and the `n_mw`/`n_lli` counters key on the detection, refloat accounting is a
separate warm-only counter. A cold run writes `mw_wl_slip`/`lli_slip` rows with `action=diagnostic`.
Measured on the golden 11-epoch run: **67 wide-lane + 87 LLI detections that previously wrote nothing
at all**; `slip refloats` stayed at **2 318** — E3's no-op verified against the actual carry counter,
not asserted.

**Card R (`network.cycle_slip.if_phase_td_ref_*`).** The legacy gate (`|median(non-ref ΔΔ)| >
0.5·λ_IF`, `n ≥ 2`, fixed) is kept as the **candidate filter** (`IfPhaseSlipDetector.cpp:617`); when
armed, `evaluateRefGate` (`:399-518`) adjudicates in five parts.
| part | rule |
|---|---|
| 1 | **Scale** — `\|median\| > max(floor_frac·λ_IF, k·s_eff)`, `s_eff` an empirical-Bayes blend of the constellation's EWMA of non-reference DD **dispersion** with a cold prior of `floor_frac·λ_IF / k` (deliberately *not* this epoch's dispersion — F5). Zero empirical information ⇒ the gate degenerates to exactly the legacy physical floor. **INVARIANT: the dispersion EWMA (`updateRefDispersion`, `:351-397`) advances on EVERY epoch with a usable family, not only candidate epochs**, so a persistent drift cannot raise its own acceptance threshold. |
| 2 | **Coherence** — a reference slip is a *common* step: `MAD ≤ coherence·\|median\|`. |
| 3 | **Incoherent split (the F5 fix)** — incoherent families split on whether members are *independently* slip-sized (`\|dd\| > λ_IF`, a full IF cycle, **not** the half-cycle candidate floor). **≥ half the family slip-sized ⇒ STATION EVENT**: reset the reference arc (conservative under a zero-q carry, invariant #1) but **do NOT rebase `prev_grad` by the median** (`:669-671` skipped) — the median is a mixture; rebasing would inject a fictitious step into every other arc and hide the real per-satellite jumps from the pool arm. Otherwise ⇒ **drift: suppress.** *(Violating the invariant in 1 or 3 re-opens a fixed bug; 3 preserves the OWMG multi-sat re-lock.)* |
| 4 | **Thin family** — below `min_n` non-reference DDs **the median is untrustworthy**, so the physical floor widens by `thin_widen` and the deferral is counted. |
| 5 | **Persistence cap** — after `max_consecutive` consecutive fires on one reference the gate stops firing and forces a reference **re-selection** (`ref_force_reselect`, `:306`, excluding the incumbent for that one election), moving the drifting satellite into the pool arm. The 9-consecutive pattern is structurally impossible. |

| card | as built |
|---|---|
| **Card L** (`network.cycle_slip.mw_lli_corroborate`) | An LLI assertion **acts** when (a) an empirical MW or GF step corroborates it — decisive at *any* station, keeping a real multi-satellite re-lock intact (M3) — or (b) the station's LLI behaviour is still credible. Only an uncorroborated assertion at a demonstrably non-credible station is downgraded, then emitted `action=suppressed` and counted (M4, `NetworkSlipQc.cpp:330-346`); because the whole `refloat_signal` call is skipped, the C3c insert obeys the same qualification as the pending-reseed path (M5). Gates empirical throughout (rule 4): per-station robust EWMA of that station's **own** \|ΔMW\| / \|ΔGF\| steps, floored by a physical bound in raw metres — **0.5 wide-lane cycles / 4 cm**, the audit's own support test — and **CLEAN SURVIVORS ONLY are folded, so the scale never absorbs the event it must catch.** *Scope limitation, counted not hidden:* an LLI asserted only on a band outside the primary dual-frequency pair is not witnessable by MW/GF; those count separately as `lli_offpair` (`:329`), **never absorbed into "uncorroborated"**. |
| **Card G** (`network.cycle_slip.gap_instrument`) | Zero mechanism change. Counts arc resumptions after ≥ 1 missed **processed** epoch; whether the raw MW/GF level shifted across the break beyond the same empirical gate; and — per arc, after the slip drains have run so the C3c set is complete and before `buildCarryDdPriors` re-keys the carry — whether the arc re-entered with **bridged** carry state (a surviving phase-leveling carry value inside `phase_leveling_max_gap_s`, or a Card-4 chord the driver still holds) versus a fresh float (`ApplicationNetwork.cpp:1881`, ledger at `:1970`). Plus G1(c): D5 re-taps tagged by raw-step coincidence. |

### 8.4 Validation (Release, same-build two-arm gate, 2026-07-21)

Config `_wlver_150sta_10ep_d10_allfreq_slipcmp.json` — 150 stations, GPS-only, `frequency_mode=all` +
`channel_mode=all`, 11 epochs emitted / 0 skipped of 12 synced.

#### 8.4.1 Baseline reproduces §0 exactly

The comparison is against a real golden, not a re-derivation: IF-TD samples → flags **8 717 → 32
(0.367 %)**; margin min/med/max **1.19 / 72.6 / 2066** (§0 said med 74); refsat-reset events **17**
(JPLM ×9, MKEA ×5, OWMG ×3); unsupported flags **14**, exactly JPLM/G10 ×9 + MKEA/G10 ×5; reverse
sweep **57 → 39**; carry slip refloats **2 318**.

#### 8.4.2 OFF gate — byte-identity (all three masters off)

53 of 57 CSV/trace artifacts **md5-identical**. `network_d5_flags.csv` + `network_wl_lane_trace.csv`
are append-mode streams whose **per-run content is md5-identical** to the golden's own tail (**1 561 /
12 569 rows**). `network_run_summary.csv` identical except the two **wall-clock** columns (**576311.5 →
505196.6 ms**) — 12 synced / 11 emitted / 0/0/0 skipped / 150 stations / 20 threads unchanged.
`network_events.csv`: pre-existing **57 rows byte-identical**, **+154 rows** every one a `T0_mw_detect`
row with `action=diagnostic`, i.e. exactly and only Card E's mandated emission. Carry census `slip
refloats 2318` **unchanged**. `main_log.log` excluded by construction (wall-clock timings).

#### 8.4.3 Unit tests

`gnss_tests.exe`: **864 / 863 passed / 1 skipped** (pre-existing fixture-gated
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`). Baseline before this work **839** ⇒ **+25 additive
cases, the first dedicated coverage the slip-detector chain has ever had.** *(Dated record — the suite
is 1360 / 196 as of 2026-08-12, CLAUDE.md.)*

#### 8.4.4 ON smoke — falsifiers

| Card | Falsifier | Result |
|---|---|---|
| E | MW/LLI rows appear with `action=diagnostic`; counters count detections | **MET** — 67 MW + 87 LLI detections, previously zero rows |
| E4 | MW ↔ IF agreement readable from `network_events.csv` alone | **MET** — grader reports 57.5 % IF↔MW match on the armed run; on the golden run that section reads "MW/LLI side is EMPTY" |
| R | pool arm unchanged | **MET** — 8 717 → 8 727 samples (sync noise), 32 → 33 flags |
| R | OWMG G31 survives | **MET** — TOW 259320, 259380, both on the new **station-event** path (reset without a mixture rebase). Card-L rows show the same re-lock epoch with all 8 satellites `LLI;mw_step` corroborated and acting |
| R | JPLM/MKEA G10 family gone | **PARTIAL — 14 → 4** (−71 %) at the shipped `max_consecutive=1`. JPLM 9→3, MKEA 5→1; longest consecutive run 1, so the 9-epoch stream is structurally impossible. Not zero: the residual fires are the *physical-floor* arm, which a cold 11-epoch run never gets past (**§8.5** — the source report's row said "see §4.5"; the mechanical map gives §8.4.5, but §8.5 Readiness is the correct target for the physical-floor-arm claim, so this pointer was deliberately corrected, not mapped) |
| L | JPN stations move to suppressed counters | **VOID** — those stations never entered the LLI arm at all (F1); the card demoted the two that genuinely chatter on bit 0 |
| L | OWMG re-locks still act | **MET** — see the R row above |
| L | carry refloats 2 318 → real-event order | **NOT MET, and not achievable by this card** — F1/F2: the assumed population does not exist, and the count is D5-dominated |
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

`slip refloats` rising (2 318 → 2 399) is expected and is why F2 matters: with the refsat false family
removed, more chords survive to be judged by D5, and D5 — not the coarse layer — sets it.

#### 8.4.4.1 Independent confirmation from the grader

`scripts/grade_slip_agreement.py` adjudicates every IF-tier flag against the raw observables **without
reading any producer counter**:

| | golden (OFF) | armed (ON) |
|---|---|---|
| IF-tier flags adjudicated | 49 | 39 |
| **raw-confirmed rate** | **71 %** | **90 %** |
| unsupported (raw series flat) | **14** — JPLM/G10 ×9, MKEA/G10 ×5 | **4** — JPLM/G10 ×3, MKEA/G10 ×1 |
| `if_slip` (pool arm) unsupported | 0 | 0 |

**The pool arm was never the problem** and is untouched; the entire unsupported population was, and
still is, the refsat side-channel — now four events with no consecutive run.

#### 8.4.5 Defaults set from the measurement, not from the spec text

`if_phase_td_ref_max_consecutive` = **1** (was 2): a genuine one-epoch common step is never consecutive
so the cap costs it nothing, while a persistent drift escalates on its second epoch instead of its
third — measured on this config, the false family goes **14 → 6 at 2, 14 → 4 at 1**, OWMG untouched
either way. `mw_lli_min_epochs` = **1**: verified safe on this data — no station demoted on epoch 1 (no
start-of-file blanket LLI), and a station demoted at first sighting has fresh arcs by construction, so
a suppression there costs the carry nothing.

#### 8.5 Readiness (2026-07-21)

The warm leg was **prepared and not launched**; it became Card W (§9.2) and was **never launched**.
Needed for: **G3's decision rule** (the shifted-AND-bridged count on a *warm* carry is the required
triage artifact — the 10-epoch smoke is cold, so almost nothing bridges by construction: necessary,
not sufficient); **Card L**'s sawtooth / D5 re-tap check at the demoted stations; and **Card R** — the
empirical dispersion EWMA only reaches full weight after ~`shrink_kappa` samples, so a 10-epoch run
exercises mostly the cold prior and the residual JPLM/MKEA events are the physical-floor arm firing,
exactly what the warm empirical arm should take over.

#### 8.6 Follow-ups this work opened (not built here)

| # | follow-up |
|---|---|
| 1 | **Re-examine the D5 innovation gate** with F2 + F3 in hand — it drives the large majority of carry slip refloats and ~97 % of its re-taps have no raw observable step; either the gate is over-firing on model tension, or "model tension" is a real slip class the observables cannot see, and `network_d5_flags.csv` plus the new raw-step tag is the instrument to decide *(became Card D5-A, §9.1/§10; **ANSWERED elsewhere — §11**)*. |
| 2 | **Widen the corroboration primitive** beyond the primary dual-frequency pair if `lli_offpair` is material on a longer/multi-constellation run (2 of 125 here — immaterial) *(became Card OP, §9.3; trigger never fired)*. |
| 3 | **Standing bit-2 (antispoofing) LLI note: nothing in the producer acts on LLI bit 2, and nothing should** — recorded so the 352-row figure is never re-derived as a defect by a future session. |

---

