# D5-A: D5 re-tap attribution, the cross-experiment protocol and Card H

**STATUS**

- **id:** D5-A
- **topic:** slip
- **status:** closed
- **summary:** Instrument LANDED, byte-identical on the solve, falsifier MET: 3.0 % of D5 re-taps coincide with an exact-epoch raw step, and counting band rows overstated the re-taps by a third.
- **dates in the text:** 2026-07-21 to 2026-08-15
- **knobs / posture:** `network.mw_wl.d5_dump` (carries the new `raw_stepped` column), `network.cycle_slip.gap_instrument` (the counters), `network.lane_carry.enable` (the pair block that doubles rows), `estimation.global_vtec_sh.rep_error_tau_s` (arm (ii) of D3); no new toggle; Card H1 `RefGateParams::max_consecutive` default 2 to 1
- **hashes named in the text** (commit / exe / byte-cert): none; exe `gnss_processor_d5a_run.exe`
- **run dirs named in the text:** none as `runs/` paths; configs `config/test_suite/_wlver_150sta_1h_d10_allfreq_{sliphard,sliphard_iono2,slipcmp}.json` and the 10-epoch `_wlver_150sta_10ep_d10_allfreq_sliphard.json`
- **source:** `docs/archive/SPEC_slip_qc_hardening.md` §10 (10.1 to 10.7; absorbed `slip_qc_d5a_report_2026-07-22.md`)

**Outcome.** The question: does the D5 innovation gate re-tap real slips or model tension? On the cold 10-epoch arm the 1 144 arc re-taps are impulsive and flat in `std_innov` across elevation: a cold reading, not a verdict. Card H aligned a gate default and split the Card-G arc ledger three ways. The D3 rule's warm leg (Card W) never ran ([slip-QC follow-ups](../slip/SLIPQC-FOLLOWUPS.md) §9.2); later work decided the question (the flags sit on chords born that epoch, [SPEC_d5_churn_limit_cycle](../../SPEC_d5_churn_limit_cycle.md) §2b), and the re-founding retired the D5 gates ([RF-1](../../refound/RF-1_float_ambiguities_design.md) section 4.1).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_slip_qc_hardening.md -->
## 10. Card D5-A + H implementation record, 2026-07-22 (absorbed `slip_qc_d5a_report_2026-07-22.md`)

Instrument-only: **no gate, no funnel membership or ordering, and no numerical path changed** —
verified by a same-config comparison against the pre-change binary (§10.5.2). The producer-side data
changes are one appended log-only CSV column plus two *counting* corrections the instrument itself
exposed (§10.4 H2, §10.2.3), both stated where they change previously-reported numbers.

#### 10.1 What landed

| item | where (HEAD, re-grepped 2026-08-15) | card |
|---|---|---|
| `raw_stepped` column on the D5 flag dump (`network.mw_wl.d5_dump` writer) | `NetworkInstallCarry.cpp:745` | D5-A/D2 |
| Grader section (g), D5 re-tap attribution, elevation carried through `build_raw_steps` | `scripts/grade_slip_agreement.py` | D5-A/D1 |
| Cross-experiment protocol + decision rule | §10.3 | D5-A/D3 |
| `RefGateParams::max_consecutive` default 2 → **1** | `include/slip/IfPhaseSlipDetector.hpp` | H/H1 |
| Card-G counter **units** explicit; arc ledger split three ways (`gap_clean_fresh`, `gap_unmeas_bridged`/`_fresh`); `arc_resume` event tag + ΔGF made to match | `include/slip/NetworkRawObservables.hpp`; `gap_clean_fresh` at `ApplicationNetwork.cpp:1970` | H/H2 |
| D5 counters gain the **arc** unit (`d5_retap_ambs`, `d5_retap_ambs_stepped`) beside the row unit | declared `include/slip/NetworkRawObservables.hpp:312-313`; ⚠ **incremented at `src/core/filter/NetworkInstallCarry.cpp:723` / `:727`** — they relocated with the `d5_dump` writer, which §9.1's ⚠ note covers only for the writer itself | D5-A/§10.2.3 |
| Defaults-drift guard + cap-generalized persistence case | `tests/test_if_phase_slip_detector.cpp` | H/H1 |
| Warm-leg configs, 3 arms, 1 h | `config/test_suite/_wlver_150sta_1h_d10_allfreq_{sliphard,sliphard_iono2,slipcmp}.json` | W |

**No new config toggle**: D2 rides the existing `d5_dump` opt-in; Card H is hygiene on shipped knobs.

### 10.2 D5-A as built

#### 10.2.1 D2 — the `raw_stepped` column (log-only, TRI-STATE)

| D2 as built |
|---|
| `network_d5_flags.csv` gains a twelfth column, `week,tow,group,chord_u,chord_v,station,amb_key,std_innov,ewl_stasat,hold_chord,injected_prev,`**`raw_stepped`**. **Deviation from the literal MUST ("0/1 from `slip_qc.epoch.stepped`"), stated:** it is **tri-state** — `1` = this (station, satellite) is in `slip_qc.epoch.stepped` (the Card-G empirical per-station MW/GF gate fired at this exact epoch); `0` = a step *could* be formed (adjacent-epoch dual-band predecessor) and was **flat**; ***blank*** = no evidence (raw observables not computed this run, arc start / gap / single-band, or the flag's ambiguity key did not resolve). **Writing `0` in the blank case would assert "no raw step" on exactly the population this card exists to decompose — a false negative on the headline statistic.** The blank follows this writer family's idiom (the lane trace already writes an empty cell for an absent quantity), and the grader treats it as unknown rather than as evidence. |
| The column reads the **same** `slip_qc.epoch` evidence the run-end `d5_retaps_stepped` counter uses, a few lines above in the same epoch block, so whenever both are armed they cannot disagree. **Arming matrix (different conditions, not a contradiction):** the column rides `network.mw_wl.d5_dump`; the counters ride `network.cycle_slip.gap_instrument`; the evidence exists only when some consumer armed the raw observables — `trace_slip_raw`, `mw_lli_corroborate` or `gap_instrument`. With `d5_dump` on and all three off, every cell is blank — the honest answer rather than a silent `0`. Numerically inert: the whole block sits inside `if (mw_d5_dump && carry_innov_gate)` and only *reads* `slip_qc.epoch`. *(D5-A/D4's scope statement — no gate/flag/funnel change — closes §10.2.3.)* |

#### 10.2.2 D1 — grader section (g), the eight-item contract

`python scripts/grade_slip_agreement.py <logdir>` gains **(g) D5 RE-TAP ATTRIBUTION** (the old
run-end-counter echo moved to (h)). No producer change; runs on existing CSVs; all of it also lands in
`--json`.

| # | item |
|---|---|
| 1 | **Dump provenance and RUN SEGMENTATION.** `network_d5_flags.csv` is append-mode, header written once *per file*, so a reused log dir holds several runs under one header — and since D2 under a header that may be one column short of the rows below. The reader segments on (a) a header line, (b) a backwards (week, tow) step, (c) a field-count change, grading the **last** segment by default (`--d5-segment last\|all\|N`). **Without this a cross-check against run-end counters — which describe only the last run — manufactures a disagreement out of good data.** |
| 2 | **Raw-step coincidence, two independent ways**: the producer's exact-epoch `raw_stepped` census, and an offline join against `network_slip_raw.csv` at ±`--epoch-tol` epochs on the grader's own MW/GF/LLI gates. Prints the coincident fraction over all flags and over flags with a series, flagging the <10 % regime explicitly as "firing on model tension". |
| 3 | **Cross-check vs run-end counters** scraped from `main_log.log` (`d5_retaps` / `d5_retaps_stepped`, the gate's `k`, the D5-v2 LS-VCE final σ̂₀ and its k·σ̂₀ threshold), with the two legitimate causes of a row-vs-counter delta spelled out (appended file; reverse-map misses, which the producer skips for the counter but still writes a row for). |
| 4 | **`std_innov` distribution** — min/p25/med/p75/p90/max, median margin against the k·σ̂₀ gate, grazing fraction, and the median of step-coincident vs flat rows (a genuine invisible slip class sits *higher*; a wash is evidence for tension). |
| 5 | **Top (station, sat) families** with distinct-epoch counts and **longest consecutive-run length** on the *processed*-epoch grid from `network_slip_raw.csv` (the D5 file alone holds only flagged epochs, so runs computed on it are upper bounds — stated in the output when it happens), plus the share of families with a run ≥ 3: impulsive vs sustained is the D5-v3 "innovation-persistence" decision input. |
| 6 | **Elevation profile in 10° bins** (count, stepped count, median `std_innov`) — decision input for the per-elevation-bin σ̂ candidate and the direct test of "is the tension geometry-structured?" (an iono-representation signature). |
| 7 | **Warm/cold split** at the first epoch whose `carry_warm` is 1 in `network_if_slip.csv` — **the producer's own flag, so no re-derivation of `pl_warmup` can drift from it** — falling back to `--pl-warmup N` and reporting **"not determined" rather than guessing**. |
| 8 | **Adjacency-tag census** (`hold_chord`, `injected_prev`, `ewl_stasat`) — free from existing columns, the cheapest test of whether the D5 population concentrates on lane-injected chords. |

#### 10.2.3 THE COUNTING CORRECTION — `d5_retaps` counts ROWS, not re-tapped chords

| the correction |
|---|
| Found during this card. Under the WL-3 joint pair block (`network.lane_carry.enable`, **armed in every Card-W arm**) the gate runs per band **ROW** — rows `i` and `K+i` are the two bands of chord `i` — and each flagged row appends the same `(group, chord)` to `lastInnovFlags()`. A chord flagged on both bands is counted twice, while the C3c funnel (a `std::set`) re-taps it **once**. Measured on the gate run: **1 689 flag rows = 1 144 distinct (epoch, group, chord) re-taps + 545 duplicates (32 %)**. So the 2026-07-21 headline "1 689 re-taps" — and the 3.0 % coincidence ratio's denominator — are in band-rows. **The RATIO is unaffected** (both rows carry the same verdict); **the MAGNITUDE of the carry churn was overstated by a third.** |
| Handled additively, not by rewriting history: `d5_retaps` / `d5_retaps_stepped` **keep their ROW semantics for continuity with the 2026-07-21 headline** (Doxygen says so, with the measured duplication rate); new `d5_retap_ambs` / `d5_retap_ambs_stepped` count distinct `(station, ambiguity)` inserts read straight off the funnel set's own `insert().second`, **so the "true re-tap" number cannot drift from what the funnel does**; the run-end line (`NetworkTeardown.cpp:1789-1794`) and grader §(g) print **both** units; and no gate, flag, funnel membership or ordering changed — the insert's return value was previously discarded. **D4 scope**: counter changes are additive only (`d5_retap_ambs*`; Card G's `gap_clean_fresh` / `gap_unmeas_*` under H2) — no existing counter's value or meaning moved. |

### 10.3 D3 — the cross-experiment protocol (binding, written BEFORE Card W runs)

Arms are §9.2's three. **Metric**: `d5_retaps` (run-end counter, cross-checked by grader §(g) row
count), plus the coincident fraction, the `std_innov` distribution, and the family/run decomposition —
per arm, over the *same* epoch span.

**Ruling on what "Iono-2 armed" means here (stated deviation).** Iono-2 carries two independently armable
mechanisms: **A** the dual-layer SH backbone (`layer2_max_degree`) and **B** the exact per-LOS deviation
carry (`rep_error_tau_s`). **Arm (ii) arms B ONLY** (`estimation.global_vtec_sh.rep_error_tau_s = 900`),
which is what the spec names — D3 asks whether *carrying the per-LOS representation error* absorbs the
tension, and **arming A at the same time would confound that with a change in the backbone the deviation
is measured against.** `layer2_max_degree = 4 / layer2_height_km = 1300` in the same file runs the full
card if the combined leg is wanted — noted in the config's `_comment_card_w`.

**Decision rule, recorded with the numbers.** **re-taps(ii) ≪ re-taps(i)** (order-of-magnitude class) ⇒
the tension **is** iono representation error: no D5 gate change, the D5 re-tap count becomes a standing
grading metric for Iono-2, residual still-firing population re-examined on its own. **re-taps(ii) ≈
re-taps(i)** ⇒ both the global-σ̂₀ hypothesis **and** the iono hypothesis are dead for this population ⇒
spec a **D5-v3** card from the §(g) decomposition — **do NOT build it inside this card** — candidates
being per-station or per-elevation-bin empirical σ̂ (decided by the elevation profile + top-family
concentration), innovation-persistence semantics (decided by the consecutive-run distribution), and, if a
genuinely invisible slip class emerges (raw-flat, iono-clean, sustained, `std_innov` well above the gate),
**a FINDING, not a gate defect** — reset-over-repair stands, the re-taps are justified, the card closes.
**Partial movement (a factor of ~2–3)** ⇒ report it as such and **SPLIT the population by whether it
moved; do not average the two conclusions.**

### 10.4 Card H as built

| item | as built |
|---|---|
| **H1 (N2)** | `RefGateParams::max_consecutive` default **2 → 1**, matching `SlipQcConfig::ref_max_consecutive` — the only armed path. Production behaviour is unchanged either way (`ApplicationNetworkSetup` assigns every field explicitly from config), so this is purely about what the *unit tests* and a header reader see. Doxygen carries the measured rationale (14 → 6 at 2, 14 → 4 at 1 on the audit config, OWMG untouched). Two test consequences, both landed: `PersistentCoherentStepEscalatesToReferenceReselection` runs at **BOTH caps (1 and 2)** — fire on each of the first `cap` epochs, escalate on the (cap+1)-th, re-elect on the (cap+2)-th, new reference fires on the (cap+3)-th, covering the run-length arithmetic rather than one operating point; and new **`HeaderDefaultsMatchTheShippedConfigDefaults`** pins every `RefGateParams` default against its `SlipQcConfig` twin, so this class of drift fails a test instead of waiting for a review. |
| **H2 (N3)** | The Card-G run-end line labels **every** number with its unit — RESUMPTIONS `(station, satellite)` vs ARCS `(station, satellite, signal)` — units documented on `SlipQcCounters` itself and the 2026-07-21 "11 shifted … 22 fresh" reading explained as the factor of two it is. **A SECOND DEFECT surfaced while reading the live log line and was fixed:** the arc-level "clean" cell was absorbing arcs of **UNMEASURABLE** resumptions (no pre-gap dual-band level), so a bridge across a break nobody could check was reported as clean — the `arc_resume` event even printed a fabricated `dGF=0.0000`. True split on the live run: **16 resumptions = 5 unmeasurable + 11 shifted + ZERO measured-and-unshifted** (all 7 of the "unshifted" arcs were untestable). The arc ledger now mirrors the resumption three-way split — SHIFTED / measured-and-unshifted / UNMEASURABLE, each closed as bridged + fresh (`gap_clean_fresh`, `gap_unmeas_bridged`, `gap_unmeas_fresh` added) — and the line says **in words that an unmeasurable-and-bridged arc is UNKNOWN, NOT BENIGN**. ⚠ **This bounds W1**: G3's "shifted-and-bridged ≈ 0 ⇒ defer" is only as strong as the unmeasurable-and-bridged count is small, and that number was previously invisible. Counters had no consumer beyond this log line, so the extension was free; nothing existing was renamed. |
| **H3 (N1)** | **DEFERRED, W6-gated — not built.** The refsat dispersion EWMA still folds an incoherent station event's dispersion (`updateRefDispersion` runs before the verdict is known), which can inflate the refsat threshold for ~1/α ≈ 7 epochs. Build only if W6 shows a genuine candidate suppressed inside that window; otherwise record "deferred, W6 = 0". Pool arm and D5 back it up. |


### 10.5 Validation (2026-07-22)

#### 10.5.1 Unit suite

`gnss_tests.exe` (Debug): **865 / 864 passed / 1 skipped** — the pre-existing fixture-gated
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`. Baseline before this card 864/863/1 ⇒ **+1 additive
case** (`HeaderDefaultsMatchTheShippedConfigDefaults`); the cap-generalized persistence case runs twice
inside one test. *(Dated record; the suite is 1360 / 196 as of 2026-08-12.)*

#### 10.5.2 Byte-identity — pre-change binary vs D5-A binary, same config, same build

| byte-identity record |
|---|
| An actual two-arm comparison. The `build/x64-Release/logs` tree left by the **pre-change** binary (built 2026-07-22 00:11) running `_wlver_150sta_10ep_d10_allfreq_sliphard.json` was snapshotted, then the same config re-run with the D5-A binary (Release, renamed exe `gnss_processor_d5a_run.exe`, 9.5 min, 12 synced / 11 emitted / 0 skipped / 150 stations — identical to the golden). **53 of the 54** comparable artifacts **md5-identical, 0 different**. `network_d5_flags.csv` — same **1 689** rows, **first 11 columns match ROW FOR ROW** (0 mismatches), the only delta the appended `raw_stepped` column. `network_events.csv` — **239 rows in both; exactly 7 lines differ, all `T0_gap arc_resume`**: `clean_fresh` → `unmeas_fresh` with `dGF` blanked instead of a fabricated `0.0000` (the H2 honesty fix); the other 232 rows, including every detector-tier row, byte-identical. `network_run_summary.csv` — identical except the two wall-clock columns (**500 434 → 517 720 ms**). `main_log.log` excluded by construction, plus the intended Card-G line relabel. So the D2 column and the counter work are **provably inert on the ON path**, and the *only* artifact byte that moved is the deliberate, enumerated 7-row re-tag; with `gap_instrument` off there are no `T0_gap` rows at all, so the OFF arm is untouched. ⚠ **A SEPARATE gate from §8.4.2** — different binaries; do not collapse them. |

#### 10.5.3 Card D5-A falsifier (10-ep smoke, sliphard config, `d5_dump` armed)

| falsifier record |
|---|
| *Grader `--d5` totals match the run-end counters* — **MET, exactly, on both units**: rows `d5_retaps = 1689` = CSV rows 1689 (delta 0), `d5_retaps_stepped = 50` = CSV `raw_stepped=1` count 50; arcs `d5_retap_ambs = 1144` = the grader's independently-computed distinct `(epoch, group, chord)` count 1144 (delta 0), `d5_retap_ambs_stepped = 36`. *`raw_stepped` populated* — **MET**: **1 631 × `0`, 50 × `1`, 8 × blank**; *review erratum 2026-07-22*: the blanks are flags with **no measurable exact-epoch verdict** (no adjacent-epoch dual-band predecessor at the flag epoch), **NOT reverse-map misses** — the rows-vs-counter delta of **+0** proves every flag resolved, since the counter skips unresolved flags while the dump still writes their row; correctly not claimed as "flat" either way. *OFF-toggle runs unchanged* — **MET**, §10.5.2. The 50 reproduces the hardening report's "1 689 re-taps, 50 raw-step-coincident (3.0 %)" from the *dump* rather than the counter, which is the point of D2: `network_d5_flags.csv` is now self-contained. The arc basis puts the same statistic at **36 / 1 144 = 3.1 %** — **the ratio survives de-duplication, the magnitude does not.** |

#### 10.5.4 The full COLD decomposition (arm (i) of D3)

Not a verdict — the cold arm alone cannot be one — but the instrument works and every number is
load-bearing for the standing "D5 fires on model tension" reading.

| # | finding |
|---|---|
| 1 | **Population**: 1 689 flag rows = **1 144 distinct arc re-taps** over 10 epochs and **542 distinct (station, satellite) pairs**. |
| 2 | **Exact-epoch coincidence 3.0 %** (50/1 689 rows; 36/1 144 arcs = 3.1 %) at the producer's empirical per-station gate. The **±1-epoch join at the audit's FIXED gates says 33 %** (558/1 689), witnessed by **MW = 281, GF = 161, GF+MW = 102, and only 5 by a bare LLI**. Both are printed with a note that they answer different questions — **strict floor vs permissive ceiling**. The spread is itself a measurement: a 3× epoch window plus a *fixed* 0.5-cycle MW gate sweeps in low-elevation noise excursions the per-station empirical gate rejects. |
| 3 | **ELEVATION is where the join's "steps" live**: **213 of 297** re-taps in the 10–20° bin are step-coincident, falling monotonically to **4 of 19** at 80–90°, while median `std_innov` is **FLAT at ~8 across every bin**. Low-elevation arcs are where both raw noise and iono representation error are largest — consistent with the iono-representation hypothesis and **not** with a slip class (a real slip population would show elevated `std_innov`, not the same 8). |
| 4 | **`std_innov`**: med **8.07**, p90 **18.5**, max **355.9** against a gate of k·σ̂₀ = 4 × 1.52 = **6.10**; **median margin 1.32×, 61 % within 1.5× of the gate.** Mostly *grazing* (a global σ̂₀ re-scale would move it wholesale) with a **long structured tail no scalar could absorb**. |
| 5 | **Step-coincident vs flat rows have the SAME `std_innov`** (med 8.05 vs 8.08) — evidence against "the 3 % are the real slips and the rest are noise". |
| 6 | **IMPULSIVE, not sustained**: 542 distinct families over 10 epochs, **median longest-run 1, only 4 families (1 %) with a run ≥ 3.** On the *cold* arm the D5 population is scattered rather than a persistent per-arc misfit — **the single most important number to re-measure warm**, because the Iono-2 hypothesis predicts *sustained* per-LOS families once the carry is warm. |
| 7 | **Adjacency**: only **67 of 1 689** flags sit on an EWL-fixed (station, sat) and **2** on a hold-cycle chord — the D5 population is essentially **DISJOINT from the lane machinery**, so the WL/UWL cards are not what drives it. |
| 8 | Every re-tap in this run is **COLD** (`carry_warm` never 1 across all **8 727** IF-TD samples), which the grader states explicitly — exactly why D3's decision rule is reserved for the warm leg. |

#### 10.5.5 Adversarial review of this change set

A four-lens review (spec conformance / C++ inertness / grader correctness / run mechanics) with **two
independent refuters per finding** produced **19 raw findings, of which 5 survived** and were fixed:

| # | Finding | Disposition |
|---|---|---|
| 1 | Card-G arc ledger filed **unmeasurable** resumptions under `clean_*` and called it "the intended behaviour" | fixed — three-way ledger (§10.4 H2); the live run shows all 7 such arcs were untestable, not clean |
| 2 | the `arc_resume` **event** kept the two-way tag and printed `dGF=0.0000` for unmeasurable arcs | fixed — event mirrors the three-way verdict, ΔGF blank (the 7-row delta in §10.5.2) |
| 3 | `d5_retaps` counts flag **ROWS**, double-counting a pair chord flagged on both bands | fixed additively — `d5_retap_ambs*` (§10.2.3); 1 689 rows = 1 144 arcs measured |
| 4 | grader folded `no-series` rows into the **flat** `std_innov` population, corrupting the card's decision statistic | fixed — bucketed by verdict, no-series reported separately |
| 5 | grader exited **1** ("nothing graded") after a complete section (g) when only the D5 dump was present | fixed — verified `EXIT=0` on a d5-only log dir |

**Findings 1 and 3 are the substantive ones**: both were *silently wrong numbers in the run-end
telemetry*, invisible until this card printed the ledger and the grader recomputed the population
independently.

#### 10.7 Not built, and why

**Card OP** — untriggered; its trigger is `lli_offpair` material on a warm or multi-constellation run,
or W4 > 0, and the cold GPS-only measurement was **2 of 125** assertions; re-evaluate from the Card W
numbers. **H3** — W6-gated (§10.4). **A D5-v3 mechanism** — **FORBIDDEN by D3** until the
cross-experiment rules.

---

