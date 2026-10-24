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

## 2026-10-24: user-side PPP-AR control campaign (ALGO, DOY 129, 2026-07-30)

These results were recorded only in the agent memory note `project_pppar_user_control` until 2026-10-24, and are copied here before the note was shortened to a pointer. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: the `mode: PPP` + `uncombined_enabled` posture named below as BROKEN was fixed on 2026-10-18 (commit f3b8f2c, step UB2 of card UOM; see OPEN_ITEMS_CODE_DEFECTS.md).

Ran 2026-07-30: ALGO00CAN, DOY-129, COD MGX FIN clock + OSB, GPS L1/L2, 24 h, graded against
ITRF2020 reduced to the ARP. The question: does user AR deliver anything with an external
satellite clock and phase bias (the control the network cannot run: it forces
`estimate_sat_clocks` + `estimate_sat_phase_biases`)? This memory is the campaign's only record;
`docs/network_ambiguity_resolution_design.md` and `docs/cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md`
cite it by name. Runs archived at `D:\GnssProcessor_archive\2026-08-12\runs\pppar\`.

**Recipe that fires:** `mode: PPP-RTK` (base `config/ppp_rtk_internal.json`), GPS-only,
`frequency_mode: dual` + `channel_mode: single` (⚠ the enabler: stock all/all builds IF-combo keys
and AR never fires, 0 fixes / 2880 ep), `between_satellite_sd.enabled`, `enable_partial_ar`,
`ratio_threshold: 3.0`, `estimate_sat_biases: false`, `enable_code_phase_bias: true`,
`position_q: 0.0`.

Verdicts:
- **Static: AR changes nothing.** AR 1.0 cm 3D vs float 0.9 cm, identical convergence at every
  threshold, 1064 fixes. COD OSB doubles the fix count (1064 vs 544), not the solution.
- **Kinematic (`position_q 1e2`), hold_variance sweep, 3D p50/p95 cm:** float 5.4/25.3; 1e-3
  (default, 668 fix epochs) 5.2/23.8; 1e-5 7.4/21.2; 1e-7 (125 fixes) **294/550**. The integers are
  WRONG. The default hold (sigma 3.2 cm = 0.17 L1 cyc) is 17x looser than the float's tightest NL
  direction (0.0097 cyc), so it injects nothing (siglo_ratio 0.9955). FAIL-UNSAFE: mean ratio 3533
  (up to 9540) is the tell of a degenerate search, not confidence; fix rate 12.7 % (1064/8348
  candidates). ⚠ Never take an AR fix count from this engine as evidence of integer recovery.
- **Qb collapse is NOT the mechanism:** the `QBCOND` probe (`AmbiguityResolver.cpp`) shows
  cond_ratio ~1 across a fix (the Joseph-form cure of memory note `project_ppp_ar_standard` holds). Tight
  holds degrade conditioning over time (pre_cond 1.6e3 to 1.8e5), hence the falling fix count.
- Reject path (76 % of attempts): NL float sigma 0.053 to 0.359 cyc; rounding at 0.36 is a coin flip.
- **WL anchor tested, not the cause:** `resolveWlConstrained` takes the MW DD widelane as a hard
  integer (1 WL cyc error shifts the NL target 0.438 cyc), but the engine's WL integers scored only
  +3.4 pts over noise (raw GPS WL circR 0.590), too little to explain the wrong-NL rate.
- **Arc-maturity gate `estimation.ambiguity_resolution.min_arc_info` BUILT and FALSIFIED** (default
  0, still in tree 2026-10-07): first fix at 236 min in every arm (candidates from 53 min); gate 60
  equals off; gate 120 drops 43 % of fixes, reads slightly worse, and with hold 1e-7 still diverges
  (89/175 cm H/U). Immaturity is not why the integers are wrong; do not pursue maturity gating (the
  knob stays a diagnostic). Landed with it: `refreshArcAges_` restarts on `isAmbiguityPendingReseed`,
  because `resetAmbiguity` re-inflates the variance but KEEPS the key, so presence counting called a
  freshly refloated arc mature.
- Author, 2026-07-30: "the entire reason to use AR is to help in the earlier epochs; after
  ambiguities converge there should not be any difference in float vs fixed." AR as built is absent
  when needed and wrong when present. Open: what makes the integers wrong.

Gotchas:
- ⚠ The live user path is `resolveWlConstrained` (WL/NL), not `resolve()`; `holdAmbiguities`
  early-returns unless `ar_mode == 3`; the `ambiguity_resolver` logger is a null sink unless
  `logging.enable_ambiguity_resolver: true`.
- ⚠ `mode: PPP` + `processing.uncombined_enabled: true` is BROKEN (6.1 to 6.4 m 3D, ZWD -0.46 to
  -0.81 m, where IF-PPP gets 10 cm); PPP-RTK modes force uncombined, so standalone uncombined PPP
  was never exercised. Reproducer `config/test_suite/_pppar_algo_static_gps_l1l2.json` (later the
  NlWl vehicle of `docs/cards/consumer/CONSUMER-AR-BLUNDER.md` §8.11.8).
- Grading frame: the engine estimates the ARP; an SSC truth is the monument, so an unreduced
  comparison shows DELTA H as pure Up (ALGO 0.100 m read as U +10.5 cm). Grading ruling in
  memory note `project_antenna_eccentricity_arp_frame`.
- COD MGX OSB: GPS phase biases on L1C/L1L/L1W/L1X/L2L/L2S/L2W/L2X, code C1C/C1W/C2W (32 sats) and
  C2L/C2S/C2X (25); **no GPS L5 bias**, so L1/L2 is the honest best case. With `channel_mode:
  single` + priority W, `require_bia_for_signals` drops nothing.

Later (2026-08-10, `0bee520`, card CONSUMER-AR-BLUNDER): the WL-only and NlWl paths gained an FFR
gate and an innovation screen (default ON). The FFR gate is structurally blind to systematic
mis-framing; the unguarded NlWl path reached a 1026.71 m median on this vehicle (guarded 5.46 m:
safe, not a working AR posture). The user-side NL integer question above was not re-graded.

For the network: even this best case shows no integer benefit, so the AR stack is unproven
end-to-end.
memory note `project_why_non_integer_rootcause`.

## 2026-10-24: 38-finding review of the user-side iono and AR features (2026-06-15)

These results were recorded only in the agent memory note `project_iono_ar_review_jun2026` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: grepped and still true: `IonoVariabilityMonitor::resetSatellite` has no callers, the IPP latitude clamp, the `kMaxIter` 10000 cap, the AR mode 2 branches and the config modes. Not re-run: the GIM leakage and `floor_m` advice, and the `resolveWlOnly` reference-column behaviour.

38-finding adversarial review of the user-side iono and AR features (2026-06-15). No repo doc holds it; this memory is the record. Re-verify the code before acting (items re-checked 2026-10-07 unless noted).

**HOI physics verified CORRECT, do not touch:** I2/I3 coefficients (`7527*c`, `2437=3A²/8`, η=0.66), code/phase signs (phase I2 = -½ code, I3 = -⅓ code), units, `B·k̂` sign, IPP reconstruction, `cos(el)=sin(zenith)` GIM mapping, all residual-equivalent to POINT (memory note `reference_point_codebase`). The engine's I3 is MORE correct than POINT's literal code (POINT feeds TECU where SI el/m² is required, ~1e32 error): never "fix" toward POINT. HOI applies per band before the IF combination, no double count with GIM weighting.

**P0s closed and committed:** frozen-BSSD-pivot skip in `Application::applyGimIonoUpdate` (`frozen_pivots.count(...) || variance<=0`); `wl_max_frac` threaded into `resolveWlOnly` (`wl_fractions`). Lesson: a frozen-pivot state with a live index still leaks its innovation through a common-mode R block; skip by satellite AND by zero variance.

**P1, open:**
- GIM weighting biases per-(sat,signal) sat-bias states: the iono/sat-bias leakage is per satellite, in the between-sat subspace the GIM pins (floor 0.05 m), while the common-mode block frees only the per-constellation offset; POINT uses the GIM only for Q/initial-covariance shaping, never as an absolute-STEC pseudo-obs.
- GIM error becomes wrong AR integers: `applyGimIonoUpdate` runs BEFORE AR, so per-sat GIM error leaks via `P(amb,iono)` into the NL float. Mitigate: `floor_m` 0.3 to 0.5 m with AR on, or skip GIM rows for sats MW-locked this epoch.
- `IonoVariabilityMonitor::resetSatellite` is DEAD CODE (zero callers, 2026-10-07): after a slip the iono-Q GF arc straddles the discontinuity. Wire it beside `cycle_slip_manager_->resetSatellite`.
- IPP longitude uses an `asin` form that saturates (not wraps) above ~69° IPP latitude: silent wrong longitude for polar stations. ⚠ Moved (2026-10-07): now `computeIppGeometry` in `src/core/atmosphere/SphericalHarmonicIonosphere.cpp`, shared by the GIM and the SH field. Use `atan2`.
- IF-combo ambiguity keys fix a non-integer with the L1 wavelength (fail-unsafe with `ar_mode != 0` in combined IF-PPP); canonical entry in memory note `project_ppp_ar_standard`.
- `resolveWlOnly` never inflates the AR-pivot reference columns on a WL re-lock (the comment now says so correctly; behaviour unchanged): a slip on the reference side snaps over several epochs. Inflate any ref column with P>0. The AR pivot is a THIRD pivot (memory note `project_pivot_handover_flipflop_fix` Rule 0).
- WL-only (`wl_only: true`) ignores `ratio_threshold`, `ratio_upper_bound`, `nl_max_per_pair_frac` (no LAMBDA). ⚠ Corrected 2026-10-07: `min_success_rate` DOES gate WL-only since 2026-08-10 (bootstrap over distinct arcs), plus the innovation screen `wl_only_max_innov_cyc` (default 0.5 cyc), card `docs/cards/consumer/CONSUMER-AR-BLUNDER.md`.
- AR mode 2 ("instantaneous") silently behaves as mode 1 (only `==0` and `!=3` branches; no warn at load).

**P2, trace-watch:** adaptive-Q is a CHANGE detector (baseline window contains the recent one, so a sustained storm self-cancels after ~`baseline_window`, about 2 h at 30 s); GIM re-tightening every epoch blunts adaptive-Q (worse with no IONEX RMS maps, 3 TECU fallback); adaptive-Q and slip detection double-react on the same GF TD2 statistic; MLAMBDA has a silent 10000-iteration cap (`IlsCore.cpp`), so a possibly suboptimal best/second pair with no log; PerBand WL re-lock covariance reset is ~5x looser than NlWl (~67 vs 12 cyc 1σ: confirm the DD residual collapses below 0.1 cyc in one epoch); the KF post-fit forced pivot handover skips the ex-pivot SD re-seed (`was_disturbed=false`) and relies on the next pass's per-row outlier dispatch.

**AR config gotchas:**
- Both `*_AR.json` configs (`ppp_rtk_internal_AR.json`, `realtime_ppp_rtk_AR.json`) have `"mode": 0`, so AR does not run. `ppp_rtk_internal.json` has mode 1 but `channel_mode: all`, where AR never fires. The recipe that fires (`mode: PPP-RTK` on `ppp_rtk_internal.json`, AR mode 1, `frequency_mode: dual`, `channel_mode: single`) is not committed; it lives in memory note `project_pppar_user_control`, whose integers were found WRONG. ⚠ `config/test_suite/_pppar_algo_static_gps_l1l2.json` sets AR mode 1 + single channel but on `mode: PPP` + `uncombined_enabled`, the posture that memory found BROKEN (metre-level): not a working recipe.
- `ionosphere_weighting`, `adaptive_iono_process_noise`, `enable_higher_order_ionosphere` default OFF; HOI also needs `data_download.download_gim=true` (memory note `project_config_toggle_gotchas`).
- Mode-3 fix-and-hold is effectively a no-op: `holdAmbiguities` calls `buildDdPairs(state_manager, {})` with empty observations, the elevation filter drops every pair.

## 2026-10-24: antenna eccentricity and the ARP frame: the author's rulings of 2026-07-29 and 2026-10-23

These results were recorded only in the agent memory note `project_antenna_eccentricity_arp_frame` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

**Frame rule.** Published coordinates (IGS SINEX/SSC, NGS SIF, a surveyed `APPROX POSITION XYZ`)
are MONUMENT-referenced; every engine position state is the ARP (igs20.atx PCO/PCV are
ARP-relative). The bridge is RINEX `ANTENNA: DELTA H/E/N`. ⚠ RINEX order is (Up, East, North), not
ENU.

**Landed 2026-07-29 (always on; geometry, not a lever).** `Coordinates::markerToArp` /
`arpToMarker` own the convention (exact no-op at zero eccentricity; `arpToMarker` needs one
fixed-point step, since a single-shot inverse leaves 4 um on a 6 m tower);
`RinexParser::getArpPositionXyz` pairs the header fields. Wired at the RT / closed-loop consumer
seed, the baseline rover a priori (header fallback only), the closed-loop consumer truth
(`NetworkSetupResult::cl_truth_arp`), `Application.cpp` and `PppModel.cpp`. Tests:
`tests/test_antenna_eccentricity.cpp` (6 cases). The OTL/APL/pole GRID lookups are deliberately
not reduced (0.5 deg cells; eccentricity is noise there).

**Ruling (author, 2026-07-29): "Skip eccentricity. We only care about the repeatable ARP."** Do not
re-propose grader-side eccentricity, an emitted-frame reduction or a manifest eccentricity field.
The engine emits the ARP; `pyviz/gnssviz/eval/truth.py` grades against the SSC monument as-is;
absolute monument-frame accuracy is not a project metric. Refinement (author, 2026-10-23): when
PPP results are set beside the closed-loop consumer (whose truth is `cl_truth_arp`), grade them
against SSC reduced by DELTA H/E/N (ARP frame) as primary, with the monument numbers as a side
note (memory note `project-status-update-2026-10-29`); the harness default itself is unchanged. A constant ARP-vs-monument offset is
therefore EXPECTED in absolute vertical numbers (JPLM00USA H = +0.0610 m against the harness
U +5.7 cm, matched to 4 mm): a reason NOT to chase that DC, not a card to open. Pose vertical work
as repeatability or differential (memory note `project_vertical_instability_solidtide`).

**Network roster: a modelling defect, fixed (card UPO, closed 2026-09-22).** The roster was held at
MONUMENT coordinates while the observations refer to the antenna (48 of 59 field stations,
GRAZ00AUT 1.964 m). Each station's ZWD absorbed ~0.26 x H and the rest reached the satellite clocks
regionally: the WTZZ00DEU consumer's whole standing Up offset (-0.085 m to 0). Knob
`network.station_marker_to_arp` reduces the roster at setup; armed at the author's instruction
("use the arp (I thought we were already)", 2026-09-22) and pinned in both goldens since the GRB
re-base that day (re-verified 2026-10-07). The grading ruling above is unchanged. A
second roster defect: the coordinate resolver applies no post-seismic model (MIZU00JPN 1.6 m);
card SCU parked, MIZU00JPN dropped from the field roster.

Fixture facts (2024-129, 305-station set): 184 stations have |H| > 1 cm (NVSK 6.166, HARB 3.052,
MAYG 3.036, AGGO 0.410, ALBH/ALGO 0.100, JPLM 0.061 m). ⚠ The header `APPROX POSITION XYZ` is not an
audit reference (~0.45 m off at JPLM, 433 m at WUHN); settle roster provenance against SINEX/SIF.
Keep `apply_phase_shifts` OFF (memory note `project_rinex_phase_shift_convention`).

Record: `docs/refound/UPO_consumer_up_offset_2026-09-22.md`,
`docs/refound/SCU_station_coordinates_unknowns_2026-09-22.md`.
