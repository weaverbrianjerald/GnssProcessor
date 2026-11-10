# consumer AR: the consumer AR blunder, caught by the innovation screen

**STATUS**

- **id:** consumer AR
- **topic:** consumer
- **status:** closed
- **summary:** The 35 m WL-only blunder was a frame mismatch the FFR gate cannot see; the innovation screen catches it, and the undifferenced consumer posture was ADOPTED in the chain.
- **dates in the text:** none (git log dates the work to 2026-08-10 and 2026-08-11, commits `0bee520` and `fb6ae66`)
- **knobs / posture:** `wl_only_max_innov_cyc` (default 0.5 cycle), `estimation.state_ambiguity_basis`, `network.closed_loop.consumer_undifferenced` (set in `_agu63.json`)
- **hashes named in the text** (commit / exe / byte-cert): exe `gnss_processor_ffr2.exe` md5 `1760ABD54E83D1D68BFF6EB8804F0A10`; the stale copy `gnss_processor_ffr4.exe` `8CA08AEB…` against the fixed `88107F4C…`
- **run dirs named in the text:** `runs/cta4_q1e7_{ffr,ffr2}`, `runs/nlwl_guarded`, `runs/logchk`, `runs/cta4_q1e7_bssd_ref`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` §8.11 (§8.11.1 to §8.11.10)

**Outcome.** The question was why the closed-loop consumer's WL-only AR dragged the position on a fix reported as `ratio = inf`. That value is a no-search sentinel; the bootstrap FFR gate passes the fix at `p_succ = 1.0` because an arc's dispersion cannot see a systematic frame mismatch (.BIA MW widelane against the consumed phase-bias frame); the innovation screen takes the worst epoch to 7.75 m. Both guards now also protect the NlWl sibling, whose unguarded path reached a 1026.71 m median on ALGO00CAN; guarded it is safe, not a working AR posture. The logger test of §8.11.10 (`_logchk3h.json`) is OWED, with no recorded result.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
## §8.11 The consumer AR blunder — DIAGNOSED and FIXED, and the first two diagnoses were wrong

Binary `gnss_processor_ffr2.exe` md5 `1760ABD54E83D1D68BFF6EB8804F0A10`; arms
`runs/cta4_q1e7_{ffr,ffr2}`; suite **1345 pass / 1 skip** (+10 new).

### §8.11.1 What `ratio = inf` actually is — §8.8's reading was WRONG

§8.8 called it "a degenerate ratio test, not a confident fix".  It is neither.
`AmbiguityResolver.cpp` sets

```cpp
result.ratio = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran
```

on the **WL-only** path, which the closed-loop consumer runs (`AR mode 1, wl_only on`).  That
path asserts MW-derived DD widelane integers directly as Kalman pseudo-observations at
`wl_only_pseudo_var = 1e-4 cyc²` (σ ≈ 0.01 cycle); there is no LAMBDA search, so **there was
never a ratio test to be degenerate**.  An `+inf` in a confidence column reads as a spectacular
ratio — that is how the blunder passed review, including mine, twice.  The column is now
honest: `p_succ` is emitted alongside, and `ar_gate` reports `wl_only` rather than `fixed`.

### §8.11.2 The FFR gate — necessary, correct, and it does NOT catch this

The path's only acceptance gate was `wl_max_frac`, a POINT test on each arc's running residual;
the LAMBDA tier's bootstrap FFR gate (`min_success_rate = 0.999`) never ran here — the standing
decision that "fixed failure rate is the acceptance gate at BOTH tiers" was simply not
implemented on this path.  Built: `MwSlipDetector::getResolvedWlQuality()` — per-arc σ of the
WL arc mean **and its sample count** (a 2-sample arc whose samples agree reports σ ≈ 0, i.e.
success ≈ 1 on one degree of freedom); the product taken over **DISTINCT ARCS**, not DD
constraints (many DDs share the pivot arc, so a wrong pivot makes all of them wrong together
and a per-DD product would count the shared failure once per DD); the identical
`erf(1/(2√2·σ))` term the LAMBDA tier uses (pinned by a unit test against
`ils::computeBootstrapSuccessRate`) so both tiers report ONE comparable number; **fail-safe** —
a missing entry, an immature arc or a non-finite σ contributes **0**, never 1; and backward
elimination of the weakest arc, mirroring `selectFixableSubset`.

**Armed, it changed nothing: `p_succ = 1.000000` and the 35 m excursion stood.**  Not a defect
in the gate — the gate being *right* about the wrong question:

> **A bootstrap success rate bounds the RANDOM failure of rounding an arc.  It is computed from
> that arc's own dispersion, and nothing in an arc's dispersion knows which FRAME the filter is
> in.  It is therefore structurally blind to a SYSTEMATIC mis-framing.**  Every contributing arc
> was mature with σ → 0, so `p_succ = 1.0` was the correct answer: those widelanes are precisely
> determined.  They were also in the wrong frame.

### §8.11.3 The mechanism, by elimination

| candidate | ruled out by |
|---|---|
| degenerate ratio test | the sentinel above |
| low-confidence integers | `p_succ = 1.000000`, all arcs mature |
| ambiguity covariance inflation (`kAmbResetVar`) | `previous_dd_wl_` is necessarily EMPTY at the first epoch the σ-gate admits, so no reset can fire |
| `holdAmbiguities` | returns early unless `ar_mode == 3`; the consumer is mode 1 |

What remains fits every observation: the MW widelane lives in the **.BIA frame** while a
network-bias consumer's ambiguities live in the **consumed phase-bias frame** — the exact
conflict `setArIgnoreMwWl` exists to avoid.  Asserting across that gap at σ = 0.01 cycle dragged
the position 33 m in one epoch; the next epoch's observations no longer fit, were rejected, and
the covariance grew on process noise alone (σ 0.050 → 3.138 m).  **The covariance INCREASING
across a "fix" is the tell** — an information-adding update cannot do that, so the growth is a
consequence, not the cause.

### §8.11.4 The gate that does catch it — the innovation screen

`wl_only_max_innov_cyc` (default **0.5 cycle**): the direct analogue of the LAMBDA tier's
per-entry `nl_max_per_pair_frac`, which this path had no equivalent of.  A constraint whose
asserted integer disagrees with the predicted DD widelane by more than half a widelane is **no
longer a rounding question**, and is dropped rather than allowed to drag the state across a
frame gap.

| q = 1e-7, settled | med | p95 | **worst** | > 20 m | fixes |
|---|---|---|---|---|---|
| no guards (`cta4_q1e7`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`ratio=inf`) |
| + FFR only (`_ffr`) | 4.17 | 7.72 | **34.91** | 4 | 1 (`p_succ=1.0`) |
| **+ FFR + innovation (`_ffr2`)** | 4.84 | **7.65** | **7.75** | **0** | **0** |

`products.txt` is **byte-identical** across all three — the blast radius is grading-only on the
network path, which makes these three arms a clean one-knob A/B of the CONSUMER alone.  The
replay instrument is unchanged (5.52 / 7.30 / 7.50, 0 fixes): its σ never reached the arming
threshold, so the guards are correctly inert where there is nothing to guard.

### §8.11.5 ⇒ §8.8's boundary is DISSOLVED

With the consumer defect fixed, rung 3 is **better than rung 2 on every consumer statistic**
(4.84 / 7.65 / 7.75 vs 6.12 / 9.78 / 10.72), so §8.8's "boundary at rung 3" no longer stands —
it was a consumer defect the ladder exposed, exactly as §8.8 attributed it.  **What does NOT
change**: the producer-side cost is unaffected (products byte-identical), so σ̂₀_code still rises
4.126 → 4.212 and cb's variance factor 12.66 → 13.04 from rung 2 to rung 3.  **Still not a
promotion** — G4 remains the invariant-#1 blocker.

### §8.11.6 Remaining gaps, named not hidden

1. ~~**`applyMwWlConstraintsNlWl_` has BOTH holes**~~ — **CLOSED, §8.11.7.**
2. ~~**`wl_only on` may be the wrong posture entirely**~~ — **CLOSED, §8.11.9.**  Measured: it
   was, and not only in the tail — the guarded WL-only posture is ~1 m worse in the MEDIAN than
   not constraining at all.
3. ~~The consumer's `ambiguity_resolver` logger does not route to disk~~ — **FALSE, and the
   claim was mine.  See §8.11.10.**

### §8.11.7 The NlWl sibling — both guards applied

`applyMwWlConstraintsNlWl_` is the per-band sibling: it also asserts MW integers with no LAMBDA
search (same `+inf` sentinel) and had the same two holes.  Both guards now stand behind it in the
same order and semantics — **FFR first** (drop arcs whose failure rate cannot be bounded),
**innovation second** (drop assertions the float contradicts).  The order is not cosmetic: the
measured blunder passed the first with `p_succ = 1.0` and was caught only by the second, because
the two answer different questions — RANDOM rounding failure vs SYSTEMATIC frame disagreement.

**One trap this path has that `resolveWlOnly` does not.**  Its constraints are built per ARC from
the MW map, keyed by the detector's own `stats_key`, with `sig_hi` / `sig_lo` re-ordered by
FREQUENCY at construction:

```cpp
if (f_a > f_b) { c.sig_hi = sig_a; c.sig_lo = sig_b; }
else           { c.sig_hi = sig_b; c.sig_lo = sig_a; }   // <-- swapped vs the key
```

So `sat + "_" + sig_hi + "_" + sig_lo` is NOT the stats key whenever `f_a < f_b`; reconstructing
it would miss the quality map on exactly those arcs, and a miss is scored 0 by the fail-safe —
looking like a working guard while silently refusing every affected fix (or, had the fail-safe
gone the other way, silently bypassing the gate).  **The exact `stats_key` is therefore carried
verbatim on `ArcConstraint` and propagated to the DD constraint**, never rebuilt.

**Validation status: FIELD-EXERCISED as of §8.11.8.**  At ship time no config in the tree selected
`estimation.state_ambiguity_basis: nl_wl`, so the guards had no field evidence; what was
demonstrated then is that the wl_only consumer arm re-run on the binary carrying these edits
reproduces `runs/cta4_q1e7_ffr2` exactly (the consumer short-circuits to `resolveWlOnly` before
reaching the sibling), i.e. the edit provably did not reach the path this card exercises.
**Pinned in the unit suite** (`tests/test_wl_only_ffr.cpp`, 12 cases): the default-ON contract for
both guards and the FFR helper's fail-safe behaviour — flipping those defaults restores a 35 m
blunder with nothing in the logs to say so.

### §8.11.8 The NlWl guards, FIELD-EXERCISED — and the path was destroying the solution

**The vehicle.**  `_pppar_algo_static_gps_l1l2.json` — single-station static PPP on ALGO00CAN,
uncombined GPS L1/L2, external COD MGX FIN products, graded against the ITRF2020 SSC monument by
a grader independent of the engine.  `uncombined_enabled: true` is exactly NlWl's prerequisite;
adding `estimation.state_ambiguity_basis: nl_wl` routes AR through `applyMwWlConstraintsNlWl_`,
so the guards are ONE KNOB PAIR apart (`_nlwl_guarded` / `_nlwl_unguarded`), ~45 s per arm.

| 24 h static PPP, settled (hour ≥ 1) | median | p95 | worst | final |
|---|---|---|---|---|
| per-band control (basis unchanged) | **2.75 m** | 2.79 | 2.79 | 2.79 |
| NlWl, guards DISARMED (pre-fix behaviour) | **1026.71 m** | 2574.50 | 5000.37 | 2482.89 |
| NlWl, guards ARMED (shipped default) | **5.46 m** | 22.58 | 94.80 | 5.34 |

**The unguarded NlWl path does not merely risk a blunder — it DESTROYS the solution**, by three
orders of magnitude, on ordinary data with no event in it.  It had never been run.

**Why the FFR gate contributes nothing here — field-confirmed, not inferred.**  With the
diagnostics measured unconditionally, the DISARMED arm reports what the gate WOULD have seen:

| | applied sets, `P_s` | `max\|innov\|` seen | innov drops / all-rejects |
|---|---|---|---|
| disarmed | med **1.000000**, min **0.999913** | med 0.158, **max 17.13 cyc** | 0 / 0 |
| armed | med 1.000000, min 0.999969 | med 2.57, max 24.70 cyc | **372 / 142** |

**The bootstrap gate would have passed every single constraint set** — minimum over the whole 24 h
is 0.999913, above the 0.999 bar — while assertions off by up to **17.13 widelanes** were applied
at `wl_only_pseudo_var` σ ≈ 0.01 cycle.  §8.11.2's structural claim is now demonstrated twice on
independent data (the closed-loop consumer at 35 m, this at 1 km), not argued.

**⚠ SAFE ≠ GOOD.**  The guarded arm is still 2× worse than the per-band control in the body (5.46
vs 2.75 m) and 34× worse in the tail (94.80 vs 2.79) — consistent with what the path IS: it
applies MW widelane constraints and stops (`resolve()`'s own comment says the full LAMBDA NL search
with WL pre-constrained is future work).  **The guards make the NlWl posture safe to run; they do
not make it a working AR posture, and it must not be used as one.**

**Telemetry fix this A/B forced.**  `P_s` and `max|innov|` were computed INSIDE their guard blocks,
so a disarmed run reported `P_s=0.000000, max|innov|=0.0000` — zeros reading as "nothing to see"
when the truth was 17 cycles of disagreement going through unchallenged.  Same class of lie as
`ratio = +inf` reading as a perfect ratio, in code written to fix exactly that lie.  Both are now
MEASURED unconditionally and only ACTED on when armed, with the max labelled PRE-screen so it
cannot be misread as the applied set's residual.

### §8.11.9 The consumer POSTURE — measured, and the WL-only choice was a drag all along

§8.11.6 gap 2 asked whether `wl_only on` is the right posture for a network-bias consumer at all.
Measured on the q = 1e-7 arm, three postures, producer knobs identical throughout (`products.txt`
**byte-identical** in every arm — the consumer is held out and cannot reach the producer, so this
is purely a change of grading instrument):

| consumer posture | med | p95 | worst | > 20 m | fix epochs |
|---|---|---|---|---|---|
| BSSD + `ar_wl_only`, guards OFF | 4.17 | 7.72 | **34.91** | 4 | 1 (wrong) |
| BSSD + `ar_wl_only`, guards ON | 4.84 | 7.65 | 7.75 | 0 | 0 |
| **`consumer_undifferenced: true`** | **3.88** | **6.35** | **6.95** | **0** | 0 |

**The undifferenced consumer is best on all three statistics — and it never fixes**: all 350
settled epochs sit at `sigma_gate`, AR never fires, so the gain is **not asserting MW widelanes at
all**.  Middle row vs bottom: the GUARDED WL-only posture is still ~1 m worse in the median than
simply not constraining.  **The WL-only posture was a net drag on the BODY of the distribution the
whole time, not only a tail risk.**  It was also the wrong instrument on its own terms: the closed
loop exists to answer *"can a held-out station recover integers from the network's emitted phase
biases?"*, and under `ar_wl_only` the consumer never runs a narrowlane search — it asserts MW
widelanes from the **.BIA** product frame and stops, validating substantially the MW detector in a
different frame from the product under test.  `consumer_undifferenced` runs direct per-band LAMBDA
on the network-bias-corrected float, which IS the quantity the product must make integer, and
suppresses the MW feed as a consequence (`ar_wl_only_` left at default ⇒ `setArIgnoreMwWl(true)`).

**Adopted in the chain.**  `_agu63.json` now sets
`network.closed_loop.consumer_undifferenced: true`.  Verified by inheritance: the UNMODIFIED
`_cta4_q1e7_ffr2` config on the new chain reproduces the explicit undifferenced arm's
`closed_loop_trace.csv` **byte-identically**, `products.txt` still byte-identical to every other
arm.  `ar_wl_only` stays in the config, inert under the undifferenced path
(`deriveClosedLoopConsumerConfig` copies it only for a BSSD consumer), recording what the chain
used to run.  **⚠ SHARED-CHAIN EDIT** — every config extending `_agu63` inherits it.  **No product
lineage is invalidated** (products byte-identical, proven on this arm) but **stored
`closed_loop_trace.csv` numbers for other cards will not reproduce**: the grading instrument
changed, so a card re-reading old consumer numbers must re-run or note the posture change.
`_cta4_q1e7_bssd_ref.json` keeps the old posture reachable for that comparison.

**Attribution defect found and fixed in passing.**  The guarded BSSD arm showed 27 `mlambda_fail`
rows on paths where **MLAMBDA never runs** — the `ratio = +inf` mis-attribution class again.
`gate_reason = 5` → `wl_guard` is now set at all four guard-rejection returns in both trace
writers; on the re-run reference arm the 27 split **19 `wl_guard` + 8 `mlambda_fail`**, so the
single label hid two outcomes and "the 27 were guard rejections" (asserted before the arm ran) was
too strong — the residual 8 are genuine, the WL path returning before the guards when no
constraint is formed at all.

**⚠ Process failure worth recording.**  The first label check came back NEGATIVE (27
`mlambda_fail`, no `wl_guard`) though the code was correct: the arm ran `gnss_processor_ffr4.exe`
copied BEFORE the edit (`8CA08AEB…`) while the fix was in `88107F4C…`.  **A renamed exe — the
concurrent-build-lock workaround — is a SNAPSHOT: `cmake --build` updates `gnss_processor.exe` and
leaves the copy untouched**, so the arm silently tested old code with a clean run and no warning.
**Re-copy after every rebuild and hash BOTH.**  It surfaced only because the label was checked on
DATA rather than trusted from the diff.

### §8.11.10 The "broken consumer logger" — claim WITHDRAWN, cause NOT established

§8.11.6 gap 3 asserted the consumer's `ambiguity_resolver` logger does not reach disk even with
`enable_ambiguity_resolver: true`.  **Withdrawn — never established**, and so is the replacement
explanation (an inherited `warn` level).

- **Established:** the PPP arm `runs/nlwl_guarded` writes **181 KB** of resolver log while network
  arms write **0**; the network path DOES write per-logger files (same run: `gnss_config.log`
  44 KB, `main_log.log` 575 KB), so the file-sink machinery is not at fault; `_ft63_24h_conv`,
  four links up the chain, pins `levels.ambiguity_resolver: "warn"` and five other loggers with
  it; `logging.levels` IS parsed into `LoggingConfig::per_logger_levels`
  (`GnssConfigLoad.cpp:3118`) in a block with no enclosing guard.
- **NOT established, asserted prematurely twice:** that the logger is broken (original claim);
  that the inherited `warn` explains the empty file (the correction) — against which the 3 h
  diagnostic arm DID fix once, so it should have emitted one "applied N constraints" line and
  emitted none.
- **A THIRD sufficient cause invalidated the first test:** the 20-epoch check `runs/logchk` came
  back empty, but its consumer AR spent all 20 epochs in `min_epochs` (10) and `sigma_gate` (10)
  and **never reached `resolveWlOnly`**.  "Empty log file" has at least THREE sufficient causes —
  logger disabled, level pinned, code path never reached — and the first two were asserted before
  the third was eliminated.
- **The ergonomic fix is landed but UNVERIFIED — do not treat it as working.**
  `Application::loadConfiguration` emits a one-line ENABLED-BUT-SILENCED census naming every logger
  whose level sits above `info`.  Log-only (suite green at 1354 / 1 skip), but **it did not fire in
  the one run attempted** and that is unexplained: the string is in the binary and the emitting
  block has no guard, so either `per_logger_levels` was empty for that config (⇒ `logging.levels`
  does NOT deep-merge across the inheritance chain, contradicting the effective-config reading
  above) or something else.

**OWED — the decisive test** (needs a free machine; a concurrent session held the producer through
two attempts, and the shared `logs/` harvest makes overlapping runs mutually corrupting): run a
**3 h BSSD + `ar_wl_only`** arm — the one posture where the resolver PROVABLY reaches its logging
sites (19 `wl_guard` + 8 `mlambda_fail` measured in `runs/cta4_q1e7_bssd_ref`) — with **both**
`logging.enable_ambiguity_resolver: true` **and** `logging.levels.ambiguity_resolver: "info"`.
Config written: `_logchk3h.json`.  Log fills ⇒ the level was the cause and the census is the right
fix.  Stays empty ⇒ there IS a defect and the original claim was right for the wrong reason.

**This section's real finding is methodological.**  Five diagnoses here were wrong on first reading
and corrected only by measurement: `ratio = +inf` as a degenerate ratio (a sentinel), the FFR gate
as the fix (it passes at `p_succ = 1.0`), covariance inflation as the cause (a consequence), the
consumer logger as broken, then the level as its cause — plus a stale renamed exe making a correct
fix read as not-applied.  **On this card the plausible reading of a symptom was wrong essentially
every time**, and three of those readings reached a SPEC before they were caught.

---

