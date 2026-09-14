# injection bench — the G-B event-injection bench — the ambiguity N-regime, the pre-flight, the arms, and the first bench run

**STATUS**

- **id:** injection bench
- **topic:** sim-bed
- **status:** closed
- **summary:** the G-B event-injection bench — the ambiguity N-regime, the pre-flight, the arms, and the first bench run
- **dates in the text:** 2026-08-13 → 2026-08-15
- **knobs / posture:** `network.carry_invariant`, `network.cycle_slip.if_phase_td`, `network.clock_datum_sum_to_zero`, `network.clock_datum_broadcast_anchored`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/simnet24_inject_null`, `runs/simnet24_inject_`, `runs/simnet24_inject_fault`, `runs/inj_null`, `runs/inj_fault`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — §15, 15.1, 15.2, 15.3, 15.4, 15.5, 15.6, 15.7, 15.8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §15 THE EVENT-INJECTION BENCH — the G-B harness §6 called for
## (absorbed 2026-08-15 from `docs/SPEC_sim_injection_bench.md`, now attic'd)

**Status, 2026-08-13.**  Fixture N-regime LANDED + unit-tested + validated on a generated
fixture.  Slip-injection bench BUILT (arm generator, pre-flight, grader) and exercised
end-to-end on a 24-station / 120-epoch fixture.  Grader hardening (`simnet_grade.py`
product-record census) LANDED.  **RAIM re-host field gates 1–3 remain OWED — this bench
does not discharge them (§15.7).**

Companions: `docs/SPEC_raim_pool_rehost.md` (the gates), `docs/SPEC_family_gf_datum.md`,
`docs/SPEC_pivot_visibility_gauge_defect.md` (the defect §15.6's null arm discovered).
§15 is deliberately self-contained: a LIVE workstream (24 `_simnet24_inj_*` configs exist
against a record of the first arms only), so it can be split back out cheaply if it doubles.

**Anchor map for the absorbed doc's referrers** — its §1 and **§1.1/§1.2/§1.3 → §15.1**
(landed / draw properties / regime declaration, folded into one section) · **§1.4 →
§15.1's "The regime A/B" subsection** · §2 → §15.2 · §2.1 → §15.2.1 · §3 → §15.3 · §4 and
§4.1 → §15.4 · §5 → §15.5 · **§5.5 → §15.6** · §6 → §15.7 · §7 → §15.8.

### 15.1 The ambiguity N-REGIME — a fixture property that changes what an experiment can conclude

The simulator drew arc-constant integer ambiguities from `N ∈ [1e5, 1e6)`
(`src/sim/main.cpp`, the `ambN` lambda).  Not a cosmetic scale choice: it decides whether a
cold start that linearizes about zero begins **inside or outside its capture region**, and
every posture-C datum mechanism (eq34, `carry_invariant`, the seed round) targets zero.

**Measured, not assumed** — eight bundled IGS stations, DOY-129 2024, 1 h arcs,
Melbourne-Wubbena (so the estimate is iono-free and code noise averages down):

| quantity | field (IGS) | legacy sim | ratio |
|---|---|---|---|
| median \|N_WL\| = \|N1−N2\| | **28.5 cyc** | 268 127 | **9 400×** |
| median \|∇ΔN_WL\| (double difference) | **29.7 cyc** | 358 244 | **12 100×** |
| median \|N1\| (iono-biased, order only) | ~49 cyc | 505 000 | ~10 000× |

A geodetic receiver aligns its phase register to the pseudorange at lock, so the raw
ambiguity is O(10¹–10²) cycles.  **The sim was four decades wide.**

**Consequence, already paid for.**  On the 60-station bisection ladder **float beat every
eq34 arm by 34–852×** while the matched *field* arms passed structurally (480/480, consumer
float ~2.97 m).  The fixture could not grade the datum stack it was built for
(`project_eq34_bisection_ladder`: *"the 60-sta rung can only grade things eq34 does not
touch.  A field-realistic-N fixture is the fix."*)  The same km-scale draw is what makes
zero-target cold pins inject km-scale tension (§12.8.3) and what §13.8.2's Python-int rule
defends against.

**What landed.**  `include/sim/AmbiguityDraw.hpp` — the draw is now a unit-testable **free
function**, not a lambda inside a 3.5k-line synthesis.

```json
"ambiguity": { "regime": "field", "half_width_cycles": 48 }
```

| property | behaviour |
|---|---|
| `regime: "legacy"` (the **DEFAULT**) | `N ∈ [1e5,1e6)`, **byte-identical to every archived fixture**, pinned by `SimAmbiguityRegime.LegacyReproducesTheHistoricalExpression`, which re-implements the pre-extraction expression and compares over a **2 × 32 × 3 × 4** sweep |
| `regime: "field"` | draws `N ∈ [−W, +W]`, `W = half_width_cycles` |
| `half_width_cycles` WITHOUT `regime: "field"` | ⚠️ **HARD LOAD ERROR** — armed-but-inert is the failure this codebase pays for repeatedly, and here it would produce a config reading field-realistic beside a fixture still 1e5 cycles wide |
| **W = 48 calibration** | predicted i.i.d. medians ZD 24 / WL 29 / DD 38 vs measured field ~49 / 28.5 / 29.7.  **Verified on `runs/simnet24_inject_null` (25 stations): ZD 25.0, WL 30.0, DD 42.0, range exactly [−48, +48]** |

**Two draw properties must survive in EITHER regime** (both test-pinned; both easy to destroy
by "simplifying" the function).  **The station salt is load-bearing**: without it every
station receives numerically identical integers, so every between-station SD and DD is
**EXACTLY ZERO** — AR reports a **100 % fix rate on perfect integers while measuring
nothing**, invisible to every run gate; this matters *more* under the field regime, whose
narrow range makes accidental collisions look plausible (same defect class as §1.1 F2, from
the other side).  **Per-band draws are independent**: the pre-2026 formula made the wide-lane
`N1 − N2` the **constant −137 on every satellite**, so one MW solve generalized to the whole
constellation.

The hash is a **self-contained splitmix64 finalizer, not a shared-RNG draw**, so switching
regime moves the ambiguities **and nothing else** — noise, multipath and slip realizations
are untouched, which is what makes a regime A/B legitimate.  **The fixture declares its
regime**: `scenario_summary.json` gains an `"ambiguity"` block **only when the regime is not
legacy**, so archived fixtures regenerate byte-identically (the same rule `truth_slips.csv`'s
`source` column follows).  Without it a grader would have to infer the regime from
`truth_ambiguities.csv`, and an accidental mixed-regime A/B would read as an estimator result
rather than as two different datasets.

#### ⚠️ The regime A/B — measured, single-knob, and NOT yet explained

Same roster, same producer config, same seeds; the `ambiguity` block is the only difference
(its absence *is* the legacy knob).  24 solve stations, 120 epochs, float posture,
`scripts/simnet_amb_integer.py`, all-epoch window:

| channel | field med\|e\| | legacy med\|e\| | field round(e)=0 | legacy round(e)=0 |
|---|---|---|---|---|
| EWL(2,5) | 0.162 | **0.096** | 83.8 % | **94.1 %** |
| WL(1,2) | 0.942 | **0.452** | 30.0 % | **54.0 %** |
| L1C | 7.50 | 6.03 | 3.3 % | 7.8 % |
| L2W | 7.35 | 6.08 | 7.3 % | 7.0 % |
| L5Q | 6.94 | 5.99 | 5.9 % | 6.4 % |

**FIELD-REALISTIC INTEGERS GRADE WORSE ON THE LANE CHANNELS.**  Not a wrapped trace and not
a float-cancellation artifact: both arms genuinely track their own scale (legacy med
\|dd_cyc\| **4.2e5**, max 1.57e6; field **46.9**) and both dropped **0 rows** on the
grader's own `frac_cyc` consistency check.

**This does not weaken the fix and must not be read as one.**  The field regime exists
because it is *representative*, not because it is *easier*.  If field-realistic integers are
harder for the lane cascade, that is a fact about the estimator the old fixture was hiding —
the same category as §13.6's finding that the fractional-only instrument was concealing an
"off by 47.3".  Note \|frac_cyc\| median is **0.2583 (field) / 0.2489 (legacy) / 0.254 (real
IGS MW)**: **the fractional instrument reads the same in every regime while the whole-cycle
instrument separates them by 24 points.**

⚠️ **MECHANISM OWED — do not guess it into the record.**  Two untested candidates: **(a)
sampling** — ~9000 rows but only a few hundred independent arcs, so part of a 10-point gap
could be draw noise; **(b) a genuine narrow-range effect** — W = 48 gives only **97 slots
per band** and produces near-ties between arcs a 9e5-wide draw never produces.  **A matched
multi-seed repeat separates them cheaply** (vary the roster salt, hold everything else).
Until then this is a measurement, not an explanation.

### 15.2 The PRE-FLIGHT — do not inject at a dead mechanism

`scripts/simnet_injection_preflight.py <producer_config> [--target ...]` — offline,
config-only, flattened through the `extends` chain exactly as the loader does.  **Exit 0 =
every requested target LIVE; exit 1 = at least one INERT/DEGRADED.**

**Why it is mandatory.**  Under `network.carry_invariant` (Card 4) a documented set of QC
mechanisms is inert **BY CONSTRUCTION** — the producer banner says so (supersession clause
`GnssConfigLoadNetwork.cpp:6705`, §14.3): the invariant DD carry supersedes the PhaseBias
shared carry, the phase-leveling local carry, the RPB carry, and the eq34 pin-round / shift /
reref hygiene.  (⚠ An earlier draft cited `GnssConfigLoadNetwork.cpp:6548` for this banner;
that neighbourhood is the Eq-34 Card 3 single-receiver-chain banner instead — re-grepped
2026-08-15.)  Injecting
a fault at a superseded mechanism gives the worst outcome available: the run completes,
counters read zero, and the result is written up as *"the guard failed to catch it"* when
the guard was never wired.  **Paid for twice on post-fit RAIM and once on eq34 INC-2 (537
FRAMED re-hosts, byte-identical output, +419 s wall).**  ⚠️ **This is WORSE than standing
invariant #4**: there the counters are zero; here they can be **NON-ZERO AND SPECIFIC while
the actuation is inert.**

**Validated against the two known cases.**  On `_ft63_24h_conv.json` — the config
`SPEC_raim_pool_rehost.md` calls "the trap" — the checker independently reproduces the
diagnosis: `raim_postfit` **INERT** (two kills), `raim_within_epoch` **INERT**,
`slip_refloat_legacy` **DEGRADED**, `a4_withhold` **INERT**.  On the trio posture:
`raim_postfit` **INERT**, `slip_datum_station` **INERT** under eq34.

#### 15.2.1 A finding the pre-flight surfaced: A4 CANNOT FIRE on the gate-1/2 posture

`SPEC_raim_pool_rehost.md` **R1** warns that a "measure-only" arm is not emission-neutral
because σ̂₀ feeds **(a)** the A4 withhold and **(b)** the URA EWMA, and instructs a decision
before gate 2.

But A4 consumes `sigma0_adapt` (declared `NetworkSolveGlue.cpp:1103`, published at `:1777`),
computed **inside** `if (within_epoch_armed && carry_warm)` (`NetworkSolveGlue.cpp:1124` —
⚠ an earlier draft cited `NetworkSolveGlue.cpp:1102`, which was never the `if`; re-grepped
2026-08-15, same caveat about concurrent edits as §14.3) — **not** the post-fit seam's σ̂₀.
On the posture gates 1/2 prescribe (`carry_invariant` ON, `phase_leveling` OFF) `carry_warm`
is **false**, so **A4 CANNOT FIRE regardless of the re-host.**  R1 fires on
`raim_carry_invariant && raim_withhold_sigma0 > 0` without consulting `carry_warm`, so **it
warns on a posture where its (a) half is structurally unreachable.**

**Consequence for gate 2:** "does A4 start firing" is conditional on which posture the gate
runs in — with leveling OFF the emission-affecting surface reduces to the **URA EWMA
alone**.  Read `n_sigma0_withheld` and the URA column **SEPARATELY** rather than assuming
R1's (a) and (b) move together.  The pre-flight reports this explicitly rather than leaving
it to be rediscovered.

### 15.3 The ARMS

`scripts/simnet_inject_arms.py <null_fixture> <base_sim> <out_sim>`

**The null arm is synthesized FIRST and is an INPUT to the fault arm.**  A form-A scripted
slip fires only if that satellite is visible at that station at that exact epoch, and an
event that never fires is a **hard run-end failure** (`unfinished()`); on a global roster
most (station, satellite) pairs are below the horizon at any epoch — **measured 47 of 51 on
an earlier attempt.**  The generator reads the null arm's own `truth_visibility.csv` and
`truth_ambiguities.csv` and plans every event inside that intersection, so the null arm does
double duty: **false-alarm calibration AND the visibility oracle.**  Measured: **24 planned,
24 applied, zero `unfinished` failures.**

**Scripted slips touch NO RNG stream**, so the fault fixture is identical to the null
everywhere except at the injected `(station, sat, signal, epoch)` — what makes a differential
grade legitimate, and §9.6 is emphatic that G-B classes **must** be graded differentially,
because a headline recovery-floor table was RETRACTED when the null arm itself drifted
−0.464 m across the same window.

**The magnitude ladder.**  Default **±1, 2, 3, 5, 10, 50 cycles** spread across distinct
stations / satellites / epochs, so one fault run measures a **DETECTION FLOOR — the empirical
MDB — rather than a single yes/no.**  Plan-time constraints: one event per
`(station, sat, signal)`; **no two events sharing `(sat, epoch)`** (an attribution tier is
never asked to separate two simultaneous faults on one satellite); injections confined to
**`[0.35, 0.85]` of the arc**, past ignition and leaving a post-event detection window.

`--signal-mode mw-blind` injects the **same** delta on both bands of a wide-lane pair, which
cancels in the Melbourne-Wübbena combination and is **invisible to `mw_detect`**.  §9.7.6
measured exactly this null space and §9.7.10 showed `network.cycle_slip.if_phase_td`
catches it; the mode makes that discriminator reproducible on demand.

### 15.4 The GRADE, and the mandatory negative control

`scripts/simnet_inject_grade.py --null-run … --fault-run … --fault-fixture …`

**TRUTH COMES FROM THE FIXTURE, NOT THE PLAN.**  The join key is the fault fixture's
per-station `truth_slips.csv` rows with `source=scripted` — what the simulator actually
applied, at its resolved tow.  The injection manifest is read only as a cross-check; **a
planned event missing from truth is a BENCH DEFECT, reported as one and KEPT IN THE
DENOMINATOR, never silently dropped.**

| reported | detail |
|---|---|
| null-arm false alarms per tier | plus the `network_flag_rates.csv` observed-rate ÷ implied-α ratio — the null calibration the T3 anomaly needs (2.61e7× at 60 sta, 2.7e7× at 300 sta, both on truth-clean data) |
| fault-arm detection | per-injection match on `(station, sat)` within a latency window, latency in epochs.  **The signal is REPORTED but NOT REQUIRED** — a wide-lane detector legitimately flags the band pair (`detail = "L1C;L5Q"`) rather than the single band that jumped, and requiring a signal match would score a correct MW detection as a miss |
| differential tier counts | fault − null |
| the empirical MDB | detection rate per magnitude rung, and the smallest magnitude caught on every instance |
| run-end counters | scraped from the run log.  ⚠️ **NO CSV SINK** — both `NetworkRaimCiTallies` and `SlipQcCounters` are spdlog-only, and `write_run_manifest` carries neither `raim_carry_invariant` nor `_refloat`, so **an A/B harness currently cannot tell the two RAIM re-host arms apart from the manifest.**  Owed item in §15.7 |

**The mandatory negative control** (§6, G-B: *"an injected event that produces zero guard
trips is a WIRING FAILURE, not a quiet day.  The grader asserts this explicitly."*) is
implemented as a hard verdict with **its own EXIT CODE 4**, distinct from a detector that
ran and missed (**exit 0, reported PARTIAL** — a legitimate, gradeable power result).  On a
wiring failure the grader points at the pre-flight and **names the supersession class**.

### 15.5 Grader hardening — the EMPTY-PRODUCT trap

`scripts/simnet_grade.py` now censuses `products.txt` record types **BEFORE any channel
grade** and **REFUSES** a consumer verdict when **B, C and P are all zero**.  Records were
bucketed into per-epoch dicts and never tallied, so a product carrying header / epoch / S
lines and nothing else parsed cleanly, produced the expected epoch count, and reached every
channel as empty dicts that skipped silently.  On **2026-08-13 two arms of the eq34 bisection
ladder were in exactly that state**, their consumer read a flattering **0.037 m with nothing
applied**, and the conclusion *"seed_round breaks the consumer 0.037 → 585 m"* was
**PUBLISHED AND RETRACTED.**

| behaviour | detail |
|---|---|
| **refusal** when B=C=P=0 | **the numbers are WITHHELD DELIBERATELY** — a reader who can see them will quote them, and the failure mode is that they look *good*.  The `--json` payload **drops `err3d` / `up`** and carries `consumer_void_reason` instead, so a batch harness cannot bank the number either |
| **EXIT CODE 3** | the second condition in the file to move the exit code, after a missing `products.txt`.  A voided verdict is an input defect, not a grading opinion |
| ⚠️ **CAVEAT, not refusal,** when only *some* of B/C/P are absent | a clock-only product is a legitimate posture, but it changes what the consumer number means and an A/B spanning that difference must be labelled |
| two incidental finds | the parser silently dropped **480 `H` (heartbeat) records per run** (the docstring promised a `heartbeat` key that was never built), and orphan C/U/B/R/P records arriving before the first `E` are now counted |

**Verified both ways on archived arms:** `_e_eq34` (0 B/C/P) → **REFUSED, exit 3**;
`_ecis_rehost` → **unchanged, consumer 358.5474 m**, matching the recorded value exactly.

### 15.6 MEASURED — the first bench run

Fixture `runs/simnet24_inject_{null,fault}`, **24 solve + 1 consumer, 120 epochs (1 h @
30 s), field-N regime, float posture** (`_simnet60_m_c_tri_slant` template,
`phase_leveling` ON, `carry_invariant` OFF — **deliberately a posture the pre-flight clears
as fully LIVE**, so the bench itself was validated against a known-live mechanism before
being pointed at a contested one).  Release only, renamed exe.  **29 s per producer arm** —
the whole loop is minutes, as intended.

**Detection.**

- **22 / 24 attributable = 91.7 %**, and **0 of those 22 coincide with a null-arm flag** at
  the same (station, sat, window), so the rate needs no false-alarm discount.
- **Latency 0 epochs on all 22**; every match carried `signal_match = True` and
  `action = refloat` — the detector named the exact injected signal and acted on it.
- **Empirical MDB = 2 cycles**, the smallest magnitude caught on every instance; **±1 was
  caught 3/4.**  Differential tier counts fault − null: **T0_mw_detect +19, T1_if_slip
  +14**; run-end counters agree — null **32 detections / 0 refloats**, fault **51 / 19**.
- **The 2 misses**: a **+50 cyc on G20 L2W at 50° elevation** and a **+1 cyc on G17 L5Q at
  22°**.  The missed +50 is the interesting one and is a **POWER question, not a wiring
  one** — the bench's job is to hand that over cleanly, not to explain it.

**Truth recovery in whole cycles, null vs fault.**

| channel | null round(e)=0 | fault round(e)=0 | null med\|e\| | fault med\|e\| |
|---|---|---|---|---|
| EWL(2,5) | 83.8 % | 84.3 % | 0.162 | 0.151 |
| WL(1,2) | 30.0 % | 31.1 % | 0.942 | 0.888 |
| L1C | 3.3 % | 3.9 % | 7.50 | 6.88 |

**24 injected slips of ±1…±50 cycles produced NO MEASURABLE DEGRADATION in DD integer
recovery** — the fault arm is marginally *better* on every channel.  That is the response
mechanism working, graded against truth **in cycles rather than fractionally**, which the
field's own instrument (\|frac_cyc\| ≈ 0.25 uniform) structurally cannot do.

**Null calibration — the T3 anomaly reproduces a THIRD time.**  The null arm is **not
quiet**: **100 flags on truth-clean data** (`cycle_slips.rate_base = 0.0`, so there is not
one random slip in the fixture).

| tier | opportunities | flags | observed rate | implied α | ratio |
|---|---|---|---|---|---|
| T3 post-fit RAIM cell | 24 238 | 1 810 | 7.47e-2 | 1.973e-9 | **3.78e7×** |
| T3c RAIM consensus | 1 306 | 74 | 5.67e-2 | 1.973e-9 | **2.87e7×** |
| T4a attribution ARC | 24 238 | 50 | 2.06e-3 | 1.973e-9 | 1.05e6× |
| T2 within-epoch | 114 | 1 | 8.77e-3 | 2.700e-3 | **3.25×** |
| T1 IF-phase slip | 25 269 | 0 | 0 | 1.244e-15 | — |

This **independently reproduces** the T3 implied-α anomaly at a third scale (**2.61e7× at
60 sta, 2.7e7× at 300 sta, 3.78e7× here at 24 sta**) and, newly, **under the FIELD ambiguity
regime** — so the anomaly is **neither a station-count effect nor an artifact of the
unrealistic integer draw.**  T2 at 3.25× is the only tier whose threshold is anywhere near
its own data.  **`T0_mw_detect` fired 36 times on a fixture with ZERO slips in it** — a
false-alarm population nothing else in the harness was counting.

#### ★ The null arm's anomaly was a REAL PRODUCER DEFECT — the discovery record

The T3 tail contained one coherent excursion: **G15 at tow 303510–303570, `s_raw` up to
2214 simultaneously across six stations, on a fixture with ZERO injected faults.**  Chasing
it produced `docs/SPEC_pivot_visibility_gauge_defect.md` — which owns the root-cause record;
**this is the DISCOVERY record (the null arm found it), and the two are different facts.**

**Root cause, proven:** with `phase_datum_prioritized` OFF the network's shared phase-bias
set is `stations.front()`'s — and `stations.front()` **IS** the clock datum station.  When
the **PIVOT ALONE** loses a satellite, that satellite's phase-bias columns are removed
**NETWORK-WIDE** and the clock absorbs the released direction.  The sim truth is clean
through the whole event (windup smooth, satellite continuously visible), so it is
**entirely producer-side.**  **Causally confirmed** by a two-sided prediction test over
`clock_datum_station` — **3/3 at 24 stations including a clean negative control, then 6/6
at 60 stations** — with a **TRUTH-FREE DISCRIMINATOR: PB-only removal = avoidable defect;
PB+CLK+CB = the satellite genuinely left the network.**

Consequences for this bench: **electing a datum station that never loses a satellite cuts
the T3 false-alarm rate 7.47 % → 4.49 %** and the datum-free clock contrast **3.073 →
0.518 m**, so **~40 % of the null arm's T3 flags were downstream of an avoidable gauge
event** and the remaining 4.5 % is the threshold/population mismatch above.  **The bench's
own null arm was MIS-ELECTED**: `scripts/simnet_make_producer_config.py` warns only about
the *polar* fallback, and **visibility continuity is a second, independent criterion it does
not check** — `scripts/datum_gauge_audit.py` now does, and its `--run` mode works on field
runs.  This is the **first sim reproduction, with truth**, of a defect previously seen only
in the field as "G29 −44 m step, JPLM masked" — precisely the "does the response mechanism
work" question this bench exists to answer, arrived at from the **null arm** rather than
from an injection.

#### An error the bench caught in ITSELF

The first run planned **10 of 24 events on Galileo**.  The producer config solves **GPS
only** (its own POSTURE line reads "1 constellation(s)"), so those events landed in
observations nothing reads and **scored as detection misses** — dragging the headline to
**58.3 %** and making detection look magnitude-independent (a ±50 cyc "miss" that was never
observed).  Corrected, **GPS was 14/14 at every magnitude.**  **This is the supersession
error one layer out: INJECTING WHERE NOTHING IS LOOKING.**  Both halves are fixed —
`simnet_inject_arms.py --constellations` prevents it, and the grader detects it from the
emitted product's own satellite records and reports those events as **UNOBSERVED / excluded
from the rate**, never as misses.

### 15.7 What this bench does NOT discharge — THE single owed-list for G-B

**AN INJECTION PASS IS NECESSARY, NOT SUFFICIENT.**  The bench answers *"does the response
mechanism work"* on exact truth.  It does **not** answer *"does the event express on this
lineage"* — the field half, and the known **seeded-chain trap** where a mid-lineage
warm-resume seam measurably ERASES events under guards.  The field arms remain the
graduation gate for every sim-validated knob: `network.clock_datum_sum_to_zero`,
`raim_we_code_iono` first ON, `network.clock_datum_broadcast_anchored` + `emit_clock_disc`.

*(This list supersedes both §6's G-B brief and §9.6's closing paragraph.)*

| # | owed |
|---|---|
| 1 | **RAIM re-host field gates 1–3** (`SPEC_raim_pool_rehost.md` §7).  This bench exercises the **LEGACY** posture where the pool is `pl_amb`; the re-host onto `qc_amb` is what gates 1–3 test, and **gate 1 requires a 10-epoch `_ft63`-derived Release run with an OFF twin for byte-identity** |
| 2 | **A `carry_invariant` injection arm.**  Everything here ran on a posture the pre-flight cleared as fully LIVE, deliberately.  The interesting arm — slip injection under `carry_invariant` + `raim_carry_invariant`, where **detection is ungated but the ACTUATOR is the C3c funnel rather than the legacy refloat** — is the next card |
| 3 | **The other G-B classes.**  ⚠️ **CB step/ramp (the DOY-129 ratchet class) is currently NOT INJECTABLE AT ALL** — both bias channels are epoch-constant by construction.  Also not injectable: **receiver clock step**, **station outage**, **single-observation blunder**.  **Iono ramp IS injectable** via `iono.turb_*` / `tid_pulses`.  **Only the satellite-clock class and slips exist today** |
| 4 | **A CSV sink for the run-end counters**, plus `raim_carry_invariant` / `_refloat` in `write_run_manifest`, so an A/B harness can identify arms without scraping spdlog (§15.4) |
| 5 | **Trio-divergence stays OUT.**  Per standing instruction, **nothing is injected into the EWL-trio posture until the ~1720-cycle runaway (§13.7) is explained**; anything injected there is confounded.  `_simnet60_tri_slant_trio_480_noewl` is the existing control |

### 15.8 Reproduction

Release-only (invariant #2), renamed exe (concurrent-build lock); build per CLAUDE.md's
Build section / `scripts/vs2026_build.ps1`.

```bash
# 1. null fixture (also the visibility oracle)
python scripts/simnet_make_roster.py config/sim/simnet60_tri.json \
       config/sim/simnet24_inject.json --n 24 --consumer
#    then set trajectory.n_epochs and ambiguity.regime = "field"
build/x64-Release/gnss_simulator.exe synth-network runs/simnet24_inject_null \
       --sim-config config/sim/simnet24_inject.json

# 2. fault arm, planned against the null arm's visibility
python scripts/simnet_inject_arms.py runs/simnet24_inject_null \
       config/sim/simnet24_inject.json config/sim/simnet24_inject_fault.json \
       --ladder 1 2 3 5 10 50 --repeat-per-rung 2 --seed 7
build/x64-Release/gnss_simulator.exe synth-network runs/simnet24_inject_fault \
       --sim-config config/sim/simnet24_inject_fault.json

# 3. matched producer configs
python scripts/simnet_make_producer_config.py runs/simnet24_inject_null \
       config/test_suite/_simnet60_m_c_tri_slant.json \
       config/test_suite/_simnet24_inj_null.json \
       --products C:/GnssProcessor/runs/inj_null/products.txt --datum-station S12000SIM
#    (repeat for fault)

# 4. PRE-FLIGHT before spending run time
python scripts/simnet_injection_preflight.py config/test_suite/_simnet24_inj_fault.json \
       --target slip_detect raim_postfit raim_within_epoch

# 5. Release runs only, renamed exe, one CWD per arm
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_null.json
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_fault.json

# 6. grade
python scripts/simnet_inject_grade.py --null-run runs/inj_null --fault-run runs/inj_fault \
       --fault-fixture runs/simnet24_inject_fault \
       --manifest config/sim/simnet24_inject_fault.json.injection.json
# truth recovery in CYCLES (not fractional) is the existing integer grader:
python scripts/simnet_amb_integer.py runs/simnet24_inject_fault --run runs/inj_fault
```

---

