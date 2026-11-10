# Card D5-A + Card H — implementation report, and Card W readiness (2026-07-22)

Implements `docs/SPEC_slip_qc_followups.md` §1 (Card D5-A), §4 H1/H2 (Card H
housekeeping) and §2's preparation obligation (Card W — **prepared, NOT
launched**). Card OP (§3) is untriggered; H3 is deferred with its trigger
recorded. The parent spec's §1 ground rules
(`docs/SPEC_slip_qc_hardening.md`) are binding here unchanged.

Everything is instrument-only: **no gate, no funnel membership or ordering, and
no numerical path changed** — verified by a same-config comparison against the
pre-change binary (§5.2). The producer-side data changes are one appended
log-only CSV column, and two *counting* corrections the instrument itself
exposed: the Card-G arc ledger was filing untestable resumptions as "clean"
(§4/H2) and the D5 counter was counting flag rows as if they were re-tapped
chords (§2.3). Both are stated where they change previously-reported numbers.

---

## 1. What landed

| Area | Files | Card |
|---|---|---|
| `raw_stepped` column on the D5 flag dump | `src/core/app/ApplicationNetwork.cpp` (the `network.mw_wl.d5_dump` writer) | D5-A / D2 |
| Grader section (g) — D5 re-tap attribution | `scripts/grade_slip_agreement.py` (+ elevation carried through `build_raw_steps`) | D5-A / D1 |
| Cross-experiment protocol + decision rule | §3 of this report | D5-A / D3 |
| `RefGateParams::max_consecutive` default 2 → **1** | `include/slip/IfPhaseSlipDetector.hpp` | H / H1 |
| Card-G counter **units** made explicit; arc ledger split three ways (`gap_clean_fresh`, `gap_unmeas_bridged/_fresh`) and the `arc_resume` event tag + ΔGF made to match | `include/slip/NetworkRawObservables.hpp`, `src/core/app/ApplicationNetwork.cpp` | H / H2 |
| D5 counters gain the **arc** unit (`d5_retap_ambs`, `d5_retap_ambs_stepped`) beside the existing row unit | `include/slip/NetworkRawObservables.hpp`, `src/core/app/ApplicationNetwork.cpp` | D5-A / §2.3 |
| Defaults-drift guard + cap-generalized persistence case | `tests/test_if_phase_slip_detector.cpp` | H / H1 |
| Warm-leg configs (3 arms, 1 h) | `config/test_suite/_wlver_150sta_1h_d10_allfreq_{sliphard,sliphard_iono2,slipcmp}.json` | W |

No new config toggle: D2 rides the existing `network.mw_wl.d5_dump` opt-in
(spec D2), and Card H is hygiene on already-shipped knobs.

---

## 2. Card D5-A

### 2.1 D2 — the `raw_stepped` column (log-only, tri-state)

`network_d5_flags.csv` gains a twelfth column,
`week,tow,group,chord_u,chord_v,station,amb_key,std_innov,ewl_stasat,hold_chord,injected_prev,`**`raw_stepped`**.

**Deviation from the literal MUST, stated:** the spec says "(0/1 from
`slip_qc.epoch.stepped`)". It is written **tri-state**:

| value | meaning |
|---|---|
| `1` | this (station, satellite) is in `slip_qc.epoch.stepped` — the Card-G empirical per-station MW/GF gate fired at this exact epoch |
| `0` | a step *could* be formed (adjacent-epoch dual-band predecessor) and it was **flat** |
| *(blank)* | no evidence: raw observables not computed this run, arc start / gap / single-band, or the flag's ambiguity key did not resolve |

Writing `0` in the blank case would assert "no raw step" on exactly the
population this card exists to decompose — a false negative on the headline
statistic. The blank follows the existing idiom of this writer family (the lane
trace already writes an empty cell for an absent quantity), and the grader
treats it as unknown rather than as evidence.

The column reads the **same** `slip_qc.epoch` evidence the run-end
`d5_retaps_stepped` counter uses, a few lines above in the same epoch block, so
whenever both are armed they cannot disagree. (They have *different* arming
conditions, which is not a contradiction but is worth knowing: the column rides
`network.mw_wl.d5_dump`, the counters ride `network.cycle_slip.gap_instrument`,
and the evidence itself exists only when some consumer armed the raw observables
— `trace_slip_raw`, `mw_lli_corroborate` or `gap_instrument`. With `d5_dump` on
and all three of those off, every cell is blank, which is the honest answer
rather than a silent `0`.) Numerically inert: the whole block sits inside
`if (mw_d5_dump && carry_innov_gate)` and only *reads* `slip_qc.epoch`.

### 2.2 D1 — grader section (g)

`python scripts/grade_slip_agreement.py <logdir>` gains **(g) D5 RE-TAP
ATTRIBUTION** (the old run-end-counter echo moved to (h)). It needs no producer
change and runs on existing CSVs. It reports, for the re-tap population:

1. **Dump provenance and run segmentation.** `network_d5_flags.csv` is opened
   in append mode and its header is written once *per file*, so a log directory
   reused across runs holds several runs under one header — and, since D2, under
   a header that may be one column short of the rows below it. The reader splits
   the file into run segments on (a) a header line, (b) a backwards (week, tow)
   step, (c) a field-count change, and grades the **last** segment by default
   (`--d5-segment last|all|N`). Without this, a cross-check against run-end
   counters — which describe only the last run — would manufacture a
   disagreement out of good data.
2. **Raw-step coincidence**, two independent ways: the producer's exact-epoch
   `raw_stepped` census, and an offline join against `network_slip_raw.csv` at
   ±`--epoch-tol` epochs using the grader's own MW/GF/LLI gates. Prints the
   coincident fraction over all flags and over flags with a series, and flags
   the <10 % regime explicitly as "firing on model tension".
3. **Cross-check vs the run-end counters** scraped from `main_log.log`
   (`d5_retaps` / `d5_retaps_stepped`, the gate's `k`, and the D5-v2 LS-VCE
   final σ̂₀ and its k·σ̂₀ threshold), with the two legitimate reasons for a
   row-vs-counter delta spelled out (appended file; reverse-map misses, which
   the producer skips for the counter but still writes a row for).
4. **`std_innov` distribution** — min/p25/med/p75/p90/max, the median margin
   against the k·σ̂₀ gate, the grazing fraction, and the median of the
   step-coincident rows vs the flat rows (a genuine invisible slip class should
   sit *higher* than the flat population; a wash is evidence for tension).
5. **Top (station, sat) families** with distinct-epoch counts and **longest
   consecutive-run length**, measured on the *processed*-epoch grid from
   `network_slip_raw.csv` (the D5 file alone only holds epochs that flagged, so
   runs computed on it are upper bounds — stated in the output when it happens).
   Plus the share of families with a run ≥ 3: impulsive vs sustained is the
   D5-v3 "innovation-persistence" candidate's decision input.
6. **Elevation profile** (10° bins: count, stepped count, median `std_innov`) —
   the decision input for the per-elevation-bin σ̂ candidate, and the direct test
   of "is the tension geometry-structured?" (an iono-representation signature).
7. **Warm/cold split** at the first epoch whose `carry_warm` is 1 in
   `network_if_slip.csv` (the producer's own flag, so no re-derivation of
   `pl_warmup` can drift from it), falling back to `--pl-warmup N`, and
   reporting "not determined" rather than guessing.
8. Adjacency-tag census (`hold_chord`, `injected_prev`, `ewl_stasat`) — free
   from the existing columns, and the cheapest test of whether the D5 population
   concentrates on lane-injected chords.

All of it lands in `--json` too.

### 2.3 The D5 counters were counting the wrong unit (found during this card)

`d5_retaps` counts **flag rows**, not re-tapped chords. Under the WL-3 joint
pair block (`network.lane_carry.enable`, armed in every Card-W arm) the gate runs
per band *row* — rows `i` and `K+i` are the two bands of chord `i` — and each
flagged row appends the same `(group, chord)` to `lastInnovFlags()`. A chord
flagged on both bands is therefore counted twice, while the C3c funnel (a
`std::set`) re-taps it **once**.

Measured on the gate run: **1 689 flag rows = 1 144 distinct (epoch, group,
chord) re-taps + 545 duplicates (32 %)**. So the 2026-07-21 headline "1 689
re-taps" — and the 3.0 % coincidence ratio's denominator — are in band-rows.
The ratio itself is unaffected (both rows of a chord carry the same verdict), but
the *magnitude* of the carry churn was overstated by a third.

Handled additively, not by rewriting history:
* `d5_retaps` / `d5_retaps_stepped` keep their row semantics (continuity with the
  hardening report) and their Doxygen now says so, with the measured duplication
  rate;
* new `d5_retap_ambs` / `d5_retap_ambs_stepped` count distinct `(station,
  ambiguity)` inserts, read straight off the funnel set's own `insert().second`
  — so the "true re-tap" number cannot drift from what the funnel does;
* the run-end line and grader section (g) print **both** units;
* no gate, flag, funnel membership or ordering changed — the insert's return
  value was previously discarded.

### 2.4 D4 — scope

No change to D5 gating, flags, funnel membership or ordering. Counter changes are
additive only (`d5_retap_ambs*` above; Card G's `gap_clean_fresh` /
`gap_unmeas_*` under H2) — no existing counter's value or meaning moved.

---

## 3. D3 — the cross-experiment protocol (binding, written BEFORE Card W runs)

The three arms, all on the 150-station GPS-only all-freq/all-channel tier, same
span, same station set:

| arm | config | what changes |
|---|---|---|
| (i) ON | `_wlver_150sta_1h_d10_allfreq_sliphard.json` | slip-QC hardening armed (Cards R/L/G), Iono-2 OFF. Doubles as Card W's ON arm. |
| (ii) Iono-2 | `_wlver_150sta_1h_d10_allfreq_sliphard_iono2.json` | (i) **plus** `estimation.global_vtec_sh.rep_error_tau_s = 900` |
| (iii) OFF | `_wlver_150sta_1h_d10_allfreq_slipcmp.json` | the three slip-QC masters absent. Card W's baseline arm. |

**Ruling on what "Iono-2 armed" means here (stated deviation).** The Iono-2 card
carries two independently armable mechanisms: **A** the dual-layer SH backbone
(`layer2_max_degree`) and **B** the exact per-LOS deviation carry
(`rep_error_tau_s`). Arm (ii) arms **B only**, which is what the spec names.
The question D3 asks is whether *carrying the per-LOS representation error*
absorbs the tension the D5 gate re-taps on; arming A at the same time would
confound the answer with a change in the backbone the deviation is measured
against. `layer2_max_degree=4 / layer2_height_km=1300` in the same file runs the
full card if the user wants the combined leg instead — noted in the config's own
`_comment_card_w`.

**Metric.** `d5_retaps` (run-end counter, cross-checked by grader §(g) row
count), plus the coincident fraction, the `std_innov` distribution, and the
family/run decomposition — per arm, over the *same* epoch span.

**Decision rule, to be recorded with the numbers:**

* **re-taps(ii) ≪ re-taps(i)** (order-of-magnitude class) ⇒ the tension **is**
  iono representation error. No D5 gate change. The D5 re-tap count becomes a
  standing grading metric for Iono-2, and the residual (still-firing) population
  is re-examined on its own.
* **re-taps(ii) ≈ re-taps(i)** ⇒ both the global-σ̂₀ hypothesis and the iono
  hypothesis are dead for this population ⇒ spec a **D5-v3** card from the
  section-(g) decomposition — do **not** build it inside this card. The
  candidates that decomposition feeds:
  * per-station or per-elevation-bin empirical σ̂ instead of one global σ̂₀ —
    decided by the elevation profile + top-family concentration;
  * innovation-persistence semantics (re-tap only after K consecutive
    exceedances) — decided by the consecutive-run distribution;
  * if a genuinely invisible slip class emerges (raw-flat, iono-clean,
    sustained, `std_innov` well above the gate) that is a **finding**, not a gate
    defect: reset-over-repair stands, the re-taps are justified, and the card
    closes.
* Partial movement (a factor of ~2–3) ⇒ report it as such and split the
  population by whether it moved; do not average the two conclusions.

---

## 4. Card H

* **H1 (N2).** `IfPhaseSlipDetector::RefGateParams::max_consecutive` default
  **2 → 1**, matching `SlipQcConfig::ref_max_consecutive` — the only armed path.
  Production behaviour is unchanged either way (`ApplicationNetworkSetup`
  assigns every field explicitly from config), so this is purely about what the
  *unit tests* and a reader of the header see. Doxygen now carries the measured
  rationale (14 → 6 at 2, 14 → 4 at 1 on the audit config, OWMG untouched).
  Two test consequences, both landed:
  * `PersistentCoherentStepEscalatesToReferenceReselection` now runs at **both**
    caps (1 and 2) — fire on each of the first `cap` epochs, escalate on the
    (cap+1)-th, re-elect on the (cap+2)-th, new reference fires on the (cap+3)-th
    — so the run-length arithmetic is covered rather than one operating point;
  * new `HeaderDefaultsMatchTheShippedConfigDefaults` pins every `RefGateParams`
    default against its `SlipQcConfig` twin, so this class of drift fails a test
    instead of waiting for a review.
* **H2 (N3).** The Card-G run-end line now labels **every** number with its unit
  — RESUMPTIONS `(station, satellite)` vs ARCS `(station, satellite, signal)` —
  and the units are documented on `SlipQcCounters` itself, with the 2026-07-21
  "11 shifted … 22 fresh" reading explained as the factor of two it is.
  **A second defect surfaced while reading the live line and was fixed:** the
  arc-level "clean" cell was absorbing arcs of **unmeasurable** resumptions
  (no pre-gap dual-band level), so a bridge across a break nobody could check
  was being reported as clean. In today's run that was all 7 of the "unshifted"
  arcs — the resumption ledger has *zero* measured-and-unshifted resumptions
  (16 = 5 unmeasurable + 11 shifted). The arc ledger now mirrors the resumption
  three-way split exactly — SHIFTED / measured-and-unshifted / UNMEASURABLE,
  each closed as bridged + fresh (`gap_clean_fresh`, `gap_unmeas_bridged`,
  `gap_unmeas_fresh` added) — and the line says in words that an
  unmeasurable-and-bridged arc is **unknown, not benign**. This matters for W1:
  G3's "shifted-and-bridged ≈ 0 ⇒ defer" ruling is only as strong as the
  unmeasurable-and-bridged count is small, and that number was previously
  invisible. Counters are uncommitted with no consumer beyond this log line, so
  the extension was free; nothing existing was renamed.
* **H3 (N1).** **Deferred, W6-gated** — not built. The refsat dispersion EWMA
  still folds an incoherent station event's dispersion (`updateRefDispersion`
  runs before the verdict is known), which can inflate the refsat threshold for
  ~1/α ≈ 7 epochs. Build only if W6 shows a genuine candidate suppressed inside
  that window; otherwise record "deferred, W6 = 0". The pool arm and D5 back
  it up meanwhile.

---

## 5. Validation

### 5.1 Unit suite

`gnss_tests.exe` (Debug): **865 tests / 864 passed / 1 skipped** — the skip is
the pre-existing fixture-gated `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`.
Baseline before this card was 864/863/1, so **+1 additive case**
(`HeaderDefaultsMatchTheShippedConfigDefaults`); the cap-generalized persistence
case runs twice inside one test.

### 5.2 Byte-identity — pre-change binary vs D5-A binary, same config, same build

Not an argument: an actual two-arm comparison. The `build/x64-Release/logs` tree
left by the **pre-change** binary (built 2026-07-22 00:11) running
`_wlver_150sta_10ep_d10_allfreq_sliphard.json` was snapshotted, then the same
config was re-run with the D5-A binary (Release, renamed exe
`gnss_processor_d5a_run.exe`, 9.5 min, 12 synced / 11 emitted / 0 skipped / 150
stations — identical to the golden).

| Comparison | Result |
|---|---|
| 53 of the 54 comparable artifacts (every CSV/trace except the ones below) | **md5-identical, 0 different** |
| `network_d5_flags.csv` | same **1 689** rows; the **first 11 columns match row for row** (0 mismatches); the only delta is the appended `raw_stepped` column |
| `network_events.csv` | 239 rows in both; **7 lines differ, all `T0_gap arc_resume`** — `clean_fresh` → `unmeas_fresh` with `dGF` blanked instead of a fabricated `0.0000` (the H2 honesty fix, §4). The other 232 rows, including every detector-tier row, are byte-identical |
| `network_run_summary.csv` | identical except the two wall-clock columns (`500 434 → 517 720 ms`) |
| `main_log.log` | excluded by construction (wall-clock timings), plus the intended Card-G line relabel |

So the D2 column and the counter work are **provably inert on the ON path**, not
merely inert-by-inspection, and the *only* artifact byte that moved is the
deliberate, enumerated 7-row re-tag. With `gap_instrument` off there are no
`T0_gap` rows at all, so the OFF arm is untouched.

### 5.3 Card D5-A falsifier (10-ep smoke, sliphard config, `d5_dump` armed)

| Falsifier clause | Result |
|---|---|
| grader `--d5` totals match the run-end counters | **MET, exactly, on both units**: rows `d5_retaps=1689` = CSV rows 1689 (delta 0), `d5_retaps_stepped=50` = CSV `raw_stepped=1` count 50; arcs `d5_retap_ambs=1144` = the grader's independently-computed distinct `(epoch, group, chord)` count 1144 (delta 0), `d5_retap_ambs_stepped=36` |
| `raw_stepped` column populated | **MET**: 1 631 × `0`, 50 × `1`, 8 × blank (review erratum 2026-07-22: the blanks are flags with **no measurable exact-epoch verdict** — no adjacent-epoch dual-band predecessor at the flag epoch — not reverse-map misses; the rows-vs-counter delta of +0 proves every flag resolved, since the counter skips unresolved flags while the dump still writes their row. Correctly not claimed as "flat" either way) |
| OFF-toggle runs unchanged | **MET** — §5.2 |

The 50 reproduces the hardening report's "1 689 re-taps, 50 raw-step-coincident
(3.0 %)" from the *dump* rather than from the counter, which is the point of D2:
`network_d5_flags.csv` is now self-contained. The arc basis puts the same
statistic at **36 / 1 144 = 3.1 %** — the ratio survives de-duplication (both
rows of a pair chord carry the same verdict), the magnitude does not.

### 5.4 What the first decomposition already says (cold arm — arm (i) of D3)

Not a verdict (the cold arm alone cannot be one), but the instrument works and
the numbers are informative:

* **Population**: 1 689 flag rows = **1 144 distinct arc re-taps** over 10
  epochs and 542 distinct (station, satellite) pairs (§2.3).
* **Exact-epoch coincidence 3.0 %** (50/1 689 rows; 36/1 144 arcs = 3.1 %) at the producer's empirical
  per-station gate. The **±1-epoch join at the audit's fixed gates** says
  **33 %** (558/1 689), witnessed by MW=281, GF=161, GF+MW=102, and only 5 by a
  bare LLI. The grader prints both with a note that they answer different
  questions — strict floor vs permissive ceiling. The spread is itself a
  measurement: a 3× epoch window plus a *fixed* 0.5-cycle MW gate sweeps in
  low-elevation noise excursions that the per-station empirical gate rejects.
* **Elevation is where the join's "steps" live**: 213 of 297 re-taps in the
  10–20° bin are step-coincident, falling monotonically to 4 of 19 at 80–90°,
  while median `std_innov` is **flat at ~8 across every bin**. Low-elevation
  arcs are where both raw noise and iono representation error are largest — so
  this is consistent with the iono-representation hypothesis and *not* with a
  slip class (a real slip population would show elevated `std_innov`, not the
  same 8).
* **`std_innov`**: med 8.07, p90 18.5, max 355.9 against a gate of k·σ̂₀ = 4 ×
  1.52 = **6.10**; median margin 1.32×, 61 % within 1.5× of the gate. So the
  population is *mostly* grazing (a global σ̂₀ re-scale would move it wholesale)
  with a long structured tail that no scalar could absorb.
* **Step-coincident vs flat rows have the same `std_innov`** (med 8.05 vs 8.08):
  the step-coincident subset is **not** a distinct, more-violent population —
  evidence against "the 3 % are the real slips and the rest are noise".
* **Impulsive, not sustained**: 542 distinct (station, sat) families over 10
  epochs, median longest-run **1**, only 4 families (1 %) with a run ≥ 3. On the
  *cold* arm the D5 population is scattered rather than a persistent per-arc
  misfit — the single most important number to re-measure warm, because the
  Iono-2 hypothesis predicts sustained per-LOS families once the carry is warm.
* **Adjacency**: only 67 of 1 689 flags sit on an EWL-fixed (station, sat) and 2
  on a hold-cycle chord — the D5 population is essentially disjoint from the
  lane machinery, so the WL/UWL cards are not what is driving it.
* Every re-tap in this run is **COLD** (`carry_warm` never 1 across all 8 727
  IF-TD samples), which the grader now states explicitly. That is exactly why
  D3's decision rule is reserved for the warm leg.

### 5.5 Adversarial review of this change set

A four-lens review (spec conformance / C++ inertness / grader correctness / run
mechanics) with two independent refuters per finding produced 19 raw findings, of
which **5 survived**. All five are fixed above and re-gated:

| # | Finding | Disposition |
|---|---|---|
| 1 | Card-G arc ledger filed **unmeasurable** resumptions under `clean_*` and called them "the intended behaviour" | fixed — three-way ledger (§4 H2); the live run shows all 7 such arcs were untestable, not clean |
| 2 | the `arc_resume` **event** kept the two-way tag and printed `dGF=0.0000` for unmeasurable arcs | fixed — event mirrors the three-way verdict, ΔGF blank (the 7-row delta in §5.2) |
| 3 | `d5_retaps` counts flag **rows**, double-counting a pair chord flagged on both bands | fixed additively — `d5_retap_ambs*` (§2.3); 1 689 rows = 1 144 arcs measured |
| 4 | grader folded `no-series` rows into the **flat** `std_innov` population, corrupting the card's decision statistic | fixed — bucketed by verdict, no-series reported separately |
| 5 | grader exited **1** ("nothing graded") after a complete section (g) when only the D5 dump was present | fixed — verified `EXIT=0` on a d5-only log dir |

Findings 1 and 3 are the substantive ones: both were *silently wrong numbers in
the run-end telemetry*, and both were invisible until this card printed the
ledger and the grader recomputed the population independently.

---

## 6. Card W — readiness (prepared, NOT launched)

Per the staged-cadence directive this leg runs **only on the user's command**.

**Arms and sequencing.** Run **ON → Iono-2 → OFF** so the user can stop after
any arm and still hold a complete result: arm (i) alone answers W1–W6, arms
(i)+(ii) answer W7/D3, and arm (iii) is only needed for W3's ON-vs-OFF
comparison.

**Cost.** Measured on today's gate run (same machine, same config, from the
section timers): **42–60 s/epoch**, of which `dd-build` alone is 37–55 s — the
all-freq / all-channel blowup of the LF-1 lineage, out of scope here. 121 epochs
at 30 s ⇒ **~85–120 min per arm** plus ~2 min of 150-station RINEX ingestion,
so ≈ 4.5–6 h for all three arms. The 10-epoch smoke twin is the same config
truncated, so nothing but the span differs.

**Run mechanics** (parent spec §6.4):
* Release only, from `build/x64-Release`, with a renamed exe copy
  (`gnss_processor_sliphard_run.exe`) so a concurrent session's linker never
  hits a locked file; on `LNK1104`, assume a concurrent build lock and retry.
* Pre-kill zombies **name-scoped to that exe only** — a `gnss_processor*`
  wildcard kills other sessions' live runs.
* `parallel_carry=true / parallel_reduce=false` is already in the configs; leave
  it.
* **Move `logs/network_d5_flags.csv` and `logs/network_wl_lane_trace.csv` aside
  before each arm** (both are append-mode). The grader can segment an appended
  file, but a fresh file makes the arm-to-arm comparison trivial.
* Copy the whole `logs/` dir to a per-arm directory immediately after each arm —
  the next arm overwrites it.

* **Rebuild Release first** — the tree carries several uncommitted workstreams,
  so the exe on disk is only as current as the last build. Then copy, then run.
* **Smoke arm (ii) for 10 epochs before committing to its 1-hour leg** — it is
  the only arm whose knob combination (`rep_error_tau_s` on this station set +
  tier) has never been run here; arms (i) and (iii) have 10-epoch twins that ran
  today. Ten epochs cost ~10 min and prove the config loads, arms
  (`Network (Iono-2/T5v2): … GM deviation ON` in the log) and does not blow the
  epoch budget.

```bash
cd C:/GnssProcessor/build/x64-Release && cmake --build . --target gnss_processor && cp gnss_processor.exe gnss_processor_sliphard_run.exe && ./gnss_processor_sliphard_run.exe ../../config/test_suite/_wlver_150sta_1h_d10_allfreq_sliphard.json
```

(the build needs the `vcvars64` wrapper from `CLAUDE.md` if the shell has not
sourced it)

**Grading, per arm:**

```bash
python scripts/grade_slip_agreement.py <arm_logdir> --json <arm>.json
```

**Gate table to fill (each MET / NOT-MET / DEFERRED-WITH-DATA):**

| gate | source | expectation |
|---|---|---|
| W1 (G3) | run-end Card-G line, warm arm: `shifted-AND-bridged` **arcs** | ≈ 0 ⇒ deferred with data, close L4; > 0 ⇒ spec the re-entry card with the rate |
| W2 (R warm) | grader §(e) unsupported count | **0** once the dispersion EWMA is warm (the 4 cold residuals were the physical-floor arm) |
| W3 (L sawtooth) | ON vs OFF at OWMG00NZL / MAC100AUS | no ambiguity-variance sawtooth; per-station D5 re-taps do not increase; credibility transitions bounded |
| W4 (N4) | suppressed LLI population vs a later raw step / IF flag within +2 ep | ≈ 0; nonzero feeds Card OP |
| W5 | grader §(e) raw-confirmed rate; §(b) IF↔MW; §(c) histogram | ≥ 90 %; no new bunching at scale |
| W6 (N1) | ref-gate DEFER lines after a station-event epoch | observational; gates H3 |
| W7 (D3) | grader §(g) per arm | the §3 decision rule, with the numbers |

Report the result in `docs/slip_qc_warm_report_<date>.md` and update the memory
files.

---

## 7. Not built (and why)

* **Card OP** — untriggered. Its trigger is `lli_offpair` material on a warm or
  multi-constellation run, or W4 > 0; the cold GPS-only measurement was 2 of 125
  assertions. Re-evaluate from the Card W numbers.
* **H3** — W6-gated (§4).
* **A D5-v3 mechanism** — forbidden by D3 until the cross-experiment rules.

## 8. Commit logistics (flagged, not done)

The slip-QC work shares `ApplicationNetwork.cpp`, `GnssConfigTypes.hpp`,
`GnssConfigLoadNetwork.cpp` and `CMakeLists.txt` with the uncommitted CB-ADM,
Iono-2 and UWL/WL-3 workstreams, so a slip-QC-only commit needs hunk-level
staging on those files; `slip/`, `tests/`, the grader and the configs are
cleanly separable. This card touched only `ApplicationNetwork.cpp` among the
shared four. **No commit without the user's go; no co-author trailer.**
