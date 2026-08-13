# SPEC — the synthetic event-injection bench (the sim half of G4)

**Status, 2026-08-13.** Fixture N-regime LANDED + unit-tested + validated on a
generated fixture. Slip-injection bench BUILT (arm generator, pre-flight, grader)
and exercised end-to-end on a 24-station / 120-epoch fixture. Grader hardening
(`simnet_grade.py` product-record census) LANDED. RAIM re-host field gates 1–3
remain OWED — this bench does not discharge them, see §6.

Companion documents: `SPEC_synthetic_minimum_network.md` §6 (G-A/G-B/G-C, which
drafted this bench), `SPEC_raim_pool_rehost.md` (the gates), `SPEC_family_gf_datum.md`.

---

## 1. The N-regime defect, and why it came first

The simulator drew arc-constant integer ambiguities from `N ∈ [1e5, 1e6)`
(`src/sim/main.cpp`, the `ambN` lambda). That is not a cosmetic scale choice: it
decides whether a cold start that linearizes about zero begins inside or outside
its capture region, and every posture-C datum mechanism (eq34, `carry_invariant`,
the seed round) targets zero.

**Measured, not assumed.** Eight bundled IGS stations, DOY-129 2024, 1 h arcs,
Melbourne-Wubbena so the estimate is iono-free and code noise averages down:

| quantity | field (IGS) | legacy sim | ratio |
|---|---|---|---|
| median \|N_WL\| = \|N1−N2\| | **28.5 cyc** | 268 127 | 9 400× |
| median \|∇ΔN_WL\| (double difference) | **29.7 cyc** | 358 244 | 12 100× |
| median \|N1\| (iono-biased, order only) | ~49 cyc | 505 000 | ~10 000× |

A geodetic receiver aligns its phase register to the pseudorange at lock, so the
raw ambiguity is O(10¹–10²) cycles. The sim was four decades wide.

**Consequence, already paid for.** On the 60-station bisection ladder float beat
every eq34 arm by 34–852× while the matched *field* arms passed structurally
(480/480, consumer float ~2.97 m). The fixture could not grade the datum stack it
was built for. Recorded in `project_eq34_bisection_ladder`: *"the 60-sta rung can
only grade things eq34 does not touch. A field-realistic-N fixture is the fix."*

### 1.1 What landed

`include/sim/AmbiguityDraw.hpp` — the draw is now a unit-testable free function,
not a lambda inside a 3.5k-line synthesis, because the regime is a *fixture
property that changes what an experiment can conclude*.

```json
"ambiguity": { "regime": "field", "half_width_cycles": 48 }
```

- `regime: "legacy"` (**default**) — `N ∈ [1e5,1e6)`, byte-identical to every
  archived fixture. Pinned by `SimAmbiguityRegime.LegacyReproducesTheHistoricalExpression`,
  which re-implements the pre-extraction expression and compares over a
  2 × 32 × 3 × 4 sweep.
- `regime: "field"` — `N ∈ [−W, +W]`, `W = half_width_cycles`.
- Setting `half_width_cycles` **without** `regime: "field"` is a hard load error.
  Armed-but-inert is the failure this codebase pays for repeatedly, and here it
  would produce a config that reads field-realistic beside a fixture that is
  still 1e5 cycles wide.

`W = 48` is calibrated against the table above. Predicted i.i.d. medians vs.
measured field: ZD 24 (field ~49, iono-biased), WL 29 (28.5), DD 38 (29.7).

**Verified on a generated fixture** (`runs/simnet24_inject_null`, 25 stations):
ZD median 25.0, **WL median 30.0**, DD median 42.0, range exactly [−48, +48].
Both structural properties survive — see §1.2.

### 1.2 Two properties the draw must preserve in EITHER regime

Both are pinned by tests; both are easy to destroy by "simplifying" the function.

1. **The station salt is load-bearing.** Without it every station receives
   numerically identical integers, so every between-station SD and DD is
   *exactly zero*: ambiguity resolution reports a 100 % fix rate on perfect
   integers while measuring nothing, and no run gate can see it. This matters
   *more* under the field regime, whose narrow range makes accidental collisions
   look plausible.
2. **Per-band draws are independent.** The pre-2026 formula made the wide-lane
   `N1−N2` the constant −137 on every satellite, so one MW solve generalized to
   the whole constellation.

The hash is a self-contained splitmix64 finalizer, not a shared-RNG draw, so
switching regime moves the ambiguities **and nothing else** — noise, multipath and
slip realizations are untouched, which is what makes a regime A/B legitimate.

### 1.3 The fixture declares its regime

`scenario_summary.json` gains an `"ambiguity"` block **only when the regime is not
legacy**, so archived fixtures regenerate byte-identically (the same rule
`truth_slips.csv`'s `source` column follows). Without it a grader would have to
infer the regime from `truth_ambiguities.csv`, and an accidental mixed-regime A/B
would read as an estimator result rather than as two different datasets.

### 1.4 ⚠ The regime A/B — measured, single-knob, and NOT yet explained

Same roster, same producer config, same seeds; the `ambiguity` block is the only
difference (its absence *is* the legacy knob). 24 solve stations, 120 epochs,
float posture, `simnet_amb_integer.py`, all-epoch window:

| channel | field med\|e\| | legacy med\|e\| | field round(e)=0 | legacy round(e)=0 |
|---|---|---|---|---|
| EWL(2,5) | 0.162 | **0.096** | 83.8 % | **94.1 %** |
| WL(1,2) | 0.942 | **0.452** | 30.0 % | **54.0 %** |
| L1C | 7.50 | 6.03 | 3.3 % | 7.8 % |
| L2W | 7.35 | 6.08 | 7.3 % | 7.0 % |
| L5Q | 6.94 | 5.99 | 5.9 % | 6.4 % |

**Field-realistic integers grade WORSE on the lane channels.** Both arms genuinely
track their own scale — legacy median \|dd_cyc\| = 4.2e5 (max 1.57e6), field 46.9 —
so this is not a wrapped trace or a float-cancellation artifact, and both arms
dropped 0 rows on the grader's own `frac_cyc` consistency check.

**This does not weaken the fix, and must not be read as one.** The field regime
exists because it is *representative*, not because it is *easier*. If
field-realistic integers are harder for the lane cascade, that is a fact about the
estimator that the old fixture was hiding — the same category as §13.6's finding
that the fractional-only instrument was concealing an "off by 47.3". Note
\|frac_cyc\| median is 0.2583 (field) and 0.2489 (legacy) — both ≈ 0.25, and the
real IGS MW measurement in §1 gives 0.254. The fractional instrument reads the same
in every regime while the whole-cycle instrument separates them by 24 points.

**Mechanism OWED — do not guess it into the record.** Two candidates, neither
tested: (a) sampling — ~9000 rows but only a few hundred independent arcs, so part
of a 10-point gap could be draw noise; (b) a genuine narrow-range effect, since
W = 48 gives only 97 slots per band and produces near-ties between arcs that a
9e5-wide draw never produces. A matched multi-seed repeat separates them cheaply
(vary `--seed`-equivalent by changing the roster salt, hold everything else).
Until then this is a measurement, not an explanation.

---

## 2. The pre-flight — do not inject at a dead mechanism

`scripts/simnet_injection_preflight.py <producer_config> [--target ...]`

Offline, config-only, flattened through the `extends` chain exactly as the loader
does. Exit 0 = every requested target LIVE; 1 = at least one INERT/DEGRADED.

**Why it is mandatory.** Under `network.carry_invariant` (Card 4) a documented set
of QC mechanisms is inert *by construction* — the producer banner says so
(`GnssConfigLoadNetwork.cpp:6548`): the invariant DD carry supersedes the
PhaseBias shared carry, the phase-leveling local carry, the RPB carry and the eq34
pin-round/shift/reref hygiene. Injecting a fault at a superseded mechanism gives
the worst outcome available: the run completes, counters read zero, and the result
is written up as *"the guard failed to catch it"* when the guard was never wired.
Paid for twice on post-fit RAIM and once on eq34 INC-2 (537 FRAMED re-hosts,
byte-identical output, +419 s wall).

This is worse than standing invariant #4: there the counters are zero, here they
can be **non-zero and specific** while the actuation is inert.

**Validated against the two known cases.** On `_ft63_24h_conv.json` — the config
`SPEC_raim_pool_rehost.md` calls "the trap" — the checker independently reproduces
the diagnosis: `raim_postfit` INERT (two kills), `raim_within_epoch` INERT,
`slip_refloat_legacy` DEGRADED, and `a4_withhold` INERT. On the trio posture it
reports `raim_postfit` INERT and `slip_datum_station` INERT under eq34.

### 2.1 A finding the pre-flight surfaced: A4 cannot fire on the gate-1/2 posture

`SPEC_raim_pool_rehost.md` R1 warns that a "measure-only" arm is not
emission-neutral because σ̂₀ feeds **(a)** the A4 withhold and **(b)** the URA EWMA,
and instructs a decision before gate 2.

But A4 consumes `sigma0_adapt`, computed **inside** `if (within_epoch_armed &&
carry_warm)` (`NetworkSolveGlue.cpp:1102`) — not the post-fit seam's σ̂₀. On the
posture gate 1/2 prescribes (`carry_invariant` ON, `phase_leveling` OFF)
`carry_warm` is false, so **A4 cannot fire regardless of the re-host**. The R1
warning fires on `raim_carry_invariant && raim_withhold_sigma0 > 0` without
consulting `carry_warm`, so it warns on a posture where its (a) half is
structurally unreachable.

**Consequence for gate 2:** "does A4 start firing" is conditional on which posture
the gate runs in — leveling OFF ⇒ the emission-affecting surface reduces to the
URA EWMA alone. Read `n_sigma0_withheld` and the URA column **separately** rather
than assuming R1's (a) and (b) move together. The pre-flight reports this
explicitly rather than leaving it to be rediscovered.

---

## 3. The arms

`scripts/simnet_inject_arms.py <null_fixture> <base_sim> <out_sim>`

**The null arm is synthesized first and is an input to the fault arm.** A form-A
scripted slip fires only if that satellite is visible at that station at that exact
epoch, and an event that never fires is a hard run-end failure (`unfinished()`); on
a global roster most (station, satellite) pairs are below the horizon at any epoch
(measured 47 of 51 on an earlier attempt). The generator therefore reads the null
arm's own `truth_visibility.csv` and `truth_ambiguities.csv` and plans every event
inside that intersection. The null arm does double duty: **false-alarm calibration
AND the visibility oracle**. Measured result: 24 planned, 24 applied, zero
`unfinished` failures.

Scripted slips touch **no RNG stream**, so the fault fixture is identical to the
null fixture everywhere except at the injected `(station, sat, signal, epoch)`.
That is what makes a differential grade legitimate — and §9.6 of
`SPEC_synthetic_minimum_network.md` is emphatic that G-B classes **must** be graded
differentially, because a headline recovery-floor table was retracted when the null
arm itself drifted −0.464 m across the same window.

**The magnitude ladder.** Default ±1, 2, 3, 5, 10, 50 cycles spread across distinct
stations/satellites/epochs, so one fault run measures a **detection floor** — the
empirical MDB — rather than a single yes/no. Constraints enforced at plan time:
one event per `(station, sat, signal)`; no two events sharing `(sat, epoch)` (an
attribution tier is never asked to separate two simultaneous faults on one
satellite); injections confined to `[0.35, 0.85]` of the arc, past ignition and
leaving a post-event detection window.

`--signal-mode mw-blind` injects the **same** delta on both bands of a wide-lane
pair, which cancels in the Melbourne-Wübbena combination and is invisible to
`mw_detect`. §9.7.6 measured exactly this null space and §9.7.10 showed
`network.cycle_slip.if_phase_td` catches it. The mode makes that discriminator
reproducible on demand.

---

## 4. The grade

`scripts/simnet_inject_grade.py --null-run … --fault-run … --fault-fixture …`

**Truth comes from the fixture, not the plan.** The join key is the fault fixture's
per-station `truth_slips.csv` rows with `source=scripted` — what the simulator
actually applied, at its resolved tow. The injection manifest is read only as a
cross-check; a planned event missing from truth is a *bench defect*, reported as
one and kept in the denominator, never silently dropped.

Reported:

- **Null arm false alarms** per tier, plus the `network_flag_rates.csv`
  observed-rate ÷ implied-α ratio. This is the null calibration the T3 anomaly
  needs (2.61e7× measured at 60 sta, 2.7e7× at 300 sta, both on truth-clean data).
- **Fault arm detection**: per-injection match on `(station, sat)` within a latency
  window, with latency in epochs. The signal is *reported but not required* —
  a wide-lane detector legitimately flags the band pair (`detail = "L1C;L5Q"`)
  rather than the single band that jumped, and requiring a signal match would
  score a correct MW detection as a miss.
- **Differential** tier counts, fault − null.
- **The empirical MDB**: detection rate per magnitude rung, and the smallest
  magnitude caught on every instance.
- **Run-end counters** scraped from the run log. Note these have **no CSV sink**:
  both `NetworkRaimCiTallies` and `SlipQcCounters` are spdlog-only, and
  `write_run_manifest` does not carry `raim_carry_invariant` /
  `_refloat` either — so an A/B harness currently cannot tell the two RAIM
  re-host arms apart from the manifest. Recorded as a gap in §6.

### 4.1 The mandatory negative control

> "an injected event that produces zero guard trips is a WIRING FAILURE, not a
> quiet day. The grader asserts this explicitly."
> — `SPEC_synthetic_minimum_network.md` §6, G-B

Implemented as a hard verdict with its own exit code (**4**), distinct from a
detector that ran and missed (exit 0, reported as PARTIAL — a legitimate,
gradeable power result). On a wiring failure the grader points at the pre-flight
and names the supersession class.

---

## 5. The grader hardening — the empty-product trap

`scripts/simnet_grade.py` now censuses products.txt record types **before any
channel grade** and **refuses** a consumer verdict when B, C and P are all zero.

Records were bucketed into per-epoch dicts and never tallied, so a product carrying
header/epoch/S lines and nothing else parsed cleanly, produced the expected epoch
count, and reached every channel as empty dicts that each skipped silently. On
2026-08-13 two arms of the eq34 bisection ladder were in exactly that state, their
consumer read a flattering **0.037 m** with nothing applied, and the conclusion
*"seed_round breaks the consumer 0.037 → 585 m"* was published and retracted.

- **Refusal** when B=C=P=0. The numbers are **withheld deliberately** — a reader
  who can see them will quote them, and the failure mode is that they look *good*.
  The `--json` payload drops `err3d`/`up` and carries `consumer_void_reason`
  instead, so a batch harness cannot bank the number either.
- **Exit code 3** — the second condition in the file to move the exit code, after
  a missing products.txt. A voided verdict is an input defect, not a grading
  opinion.
- **⚠ caveat, not refusal,** when *some* of B/C/P are absent: a clock-only product
  is a legitimate posture, but it changes what the consumer number means and an
  A/B spanning that difference must be labelled.
- Two incidental finds: the parser silently dropped **480 `H` (heartbeat) records
  per run** (the docstring promised a `heartbeat` key that was never built), and
  orphan C/U/B/R/P records arriving before the first `E` are now counted.

Verified both ways on archived arms: `_e_eq34` (0 B/C/P) → REFUSED, exit 3;
`_ecis_rehost` → unchanged, consumer 358.5474 m, matching the recorded value
exactly.

---

## 5.5 MEASURED — the first bench run

Fixture `runs/simnet24_inject_{null,fault}`, 24 solve + 1 consumer, 120 epochs
(1 h @ 30 s), field-N regime, float posture (`_simnet60_m_c_tri_slant` template,
`phase_leveling` ON, `carry_invariant` OFF — deliberately a posture the pre-flight
clears as fully LIVE, so the bench itself was validated against a known-live
mechanism before being pointed at a contested one). Release only, renamed exe.
**29 s per producer arm** — the whole loop is minutes, as intended.

### Detection

- **22 / 24 attributable = 91.7 %**, and **0 of those 22 coincide with a null-arm
  flag** at the same (station, sat, window), so the rate needs no false-alarm
  discount.
- **Latency 0 epochs** on all 22 — every detection landed in the injection epoch.
- Every match carried `signal_match = True` and `action = refloat`: the detector
  named the exact injected signal and acted on it.
- **Empirical MDB = 2 cycles** — the smallest magnitude caught on every instance.
  ±1 was caught 3/4.
- Differential tier counts fault − null: T0_mw_detect **+19**, T1_if_slip **+14**.
  Run-end counters agree: null 32 detections / **0** refloats, fault 51 / **19**.
- The 2 misses are a +50 cyc on G20 L2W at 50° elevation and a +1 cyc on G17 L5Q
  at 22°. A missed **+50** is the interesting one and is a power question, not a
  wiring one — the bench's job is to hand that over cleanly, not to explain it.

### Truth recovery in whole cycles, null vs fault

| channel | null round(e)=0 | fault round(e)=0 | null med\|e\| | fault med\|e\| |
|---|---|---|---|---|
| EWL(2,5) | 83.8 % | 84.3 % | 0.162 | 0.151 |
| WL(1,2) | 30.0 % | 31.1 % | 0.942 | 0.888 |
| L1C | 3.3 % | 3.9 % | 7.50 | 6.88 |

**24 injected slips of ±1…±50 cycles produced no measurable degradation in DD
integer recovery** — the fault arm is marginally *better* on every channel. That
is the response mechanism working, graded against truth in cycles rather than
fractionally, which is the grade the user's own instrument (`|frac_cyc| ≈ 0.25`
uniform) structurally cannot make.

### Null calibration — the T3 anomaly reproduces a third time

The null arm is **not quiet**: 100 flags on truth-clean data (`cycle_slips.rate_base
= 0.0`, so there is not one random slip in the fixture).

| tier | opportunities | flags | observed rate | implied α | ratio |
|---|---|---|---|---|---|
| T3 post-fit RAIM cell | 24 238 | 1 810 | 7.47e-2 | 1.973e-9 | **3.78e7×** |
| T3c RAIM consensus | 1 306 | 74 | 5.67e-2 | 1.973e-9 | **2.87e7×** |
| T4a attribution ARC | 24 238 | 50 | 2.06e-3 | 1.973e-9 | 1.05e6× |
| T2 within-epoch | 114 | 1 | 8.77e-3 | 2.700e-3 | 3.25× |
| T1 IF-phase slip | 25 269 | 0 | 0 | 1.244e-15 | — |

This **independently reproduces** the T3 implied-α anomaly at a third scale
(2.61e7× at 60 sta, 2.7e7× at 300 sta, 3.78e7× here at 24 sta) and, newly, under
the *field* ambiguity regime — so the anomaly is neither a station-count effect nor
an artifact of the unrealistic integer draw. T2 at 3.25× is the only tier whose
threshold is anywhere near its own data.

**T0_mw_detect fired 36 times on a fixture with zero slips in it.** That is a
false-alarm population nothing else in the harness was counting.

### An error the bench caught in itself

The first run planned 10 of 24 events on Galileo. The producer config solves **GPS
only** (its own POSTURE line reads "1 constellation(s)"), so those events landed in
observations nothing reads and scored as detection misses — dragging the headline
to 58.3 % and making detection look magnitude-independent (a ±50 cyc "miss" that
was never observed). Corrected, GPS was 14/14 at every magnitude.

This is the supersession error one layer out: *injecting where nothing is looking*.
Both halves are now fixed — `simnet_inject_arms.py --constellations` prevents it,
and the grader detects it from the emitted product's own satellite records and
reports those events as **UNOBSERVED / excluded from the rate**, never as misses.

---

## 6. What this bench does NOT discharge

**An injection PASS is NECESSARY, NOT SUFFICIENT.** The bench answers *"does the
response mechanism work"* on exact truth. It does not answer *"does the event
express on this lineage"* — the field half, and the known seeded-chain trap where a
mid-lineage warm-resume seam measurably erases events under guards. The field arms
remain the graduation gate for every sim-validated knob:
`clock_datum_sum_to_zero`, `raim_we_code_iono` first ON,
`clock_datum_broadcast_anchored` + `emit_clock_disc`.

Specifically still owed:

1. **RAIM re-host field gates 1–3** (`SPEC_raim_pool_rehost.md` §7). This bench
   exercises the *legacy* posture where the pool is `pl_amb`; the re-host onto
   `qc_amb` is what gates 1–3 test, and gate 1 requires a 10-epoch `_ft63`-derived
   Release run with an OFF twin for byte-identity.
2. **A carry_invariant injection arm.** Everything here ran on a posture the
   pre-flight cleared as fully LIVE, deliberately, so the bench itself was
   validated against a known-live mechanism first. The interesting arm — slip
   injection under `carry_invariant` + `raim_carry_invariant`, where detection is
   ungated but the *actuator* is the C3c funnel rather than the legacy refloat — is
   the next card.
3. **The other G-B classes**: CB step/ramp (the DOY-129 ratchet class — currently
   **not injectable at all**, both bias channels are epoch-constant by
   construction), receiver clock step (not injectable), station outage (not
   injectable), single-observation blunder (not injectable), iono ramp
   (injectable via `iono.turb_*` / `tid_pulses`). Only the satellite-clock class
   and slips exist today.
4. **A CSV sink for the run-end counters**, and `raim_carry_invariant` /
   `_refloat` in `write_run_manifest`, so an A/B harness can identify arms without
   scraping spdlog.
5. **Trio-divergence stays out.** Per the standing instruction, nothing is injected
   into the EWL-trio posture until the ~1720-cycle runaway is explained; anything
   injected there is confounded. `_simnet60_tri_slant_trio_480_noewl` is the
   existing control.

---

## 7. Reproduction

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

# 5. Release runs only (invariant #2), renamed exe (concurrent-build lock)
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_null.json
build/x64-Release/gnss_processor_inject.exe config/test_suite/_simnet24_inj_fault.json

# 6. grade
python scripts/simnet_inject_grade.py --null-run runs/inj_null --fault-run runs/inj_fault \
       --fault-fixture runs/simnet24_inject_fault \
       --manifest config/sim/simnet24_inject_fault.json.injection.json
# truth recovery in CYCLES (not fractional) is the existing integer grader:
python scripts/simnet_amb_integer.py runs/simnet24_inject_fault --run runs/inj_fault
```
