# DD-REF: the DD trace's reference satellite and the field re-grade

**STATUS**

- **id:** DD-REF
- **topic:** sim-bed
- **status:** closed
- **summary:** The post-eclipse network-wide DD episode was the trace's single reference satellite returning, not an estimator defect; the field histogram is NOT this artifact; `oldest` is RECOMMEND_SCOPED.
- **dates in the text:** 2026-08-13 → 2026-09-06
- **knobs / posture:** `network.trace_dd_fractional_ref` (`lowest` default; `oldest` built, RECOMMEND_SCOPED, the goldens keep `lowest`)
- **hashes named in the text** (commit / exe / byte-cert): `fe907a95`
- **run dirs named in the text:** `runs/nonint_losblock24h_hv` (sim base), `fld63_stk_on` (field), A/B arms `_ddref6h_low` / `_ddref6h_old`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` §6.12 (6.12.1 to 6.12.5), §6.13

**Outcome.** The card asked what re-realizes network-wide after an eclipse re-admission. Nothing in the estimator: every chord is differenced against one satellite (G01 / E02 all day), whose eclipse returns re-referenced the trace to newborn legs; ref-free, the 24 h L1C grade is 0.089 cyc, 86.8 % at the integer. On the field a truth-free re-grade sits at its null (R 0.468 against 0.473). The `oldest` policy improved the as-traced grade 25 to 42 % but FAILED its pre-registered bar (2.1 to 3.4x above ref-free); the [ledger](../../CONFIG_promotion_ledger.md) scopes it and the goldens keep `lowest`.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.12 The "post-eclipse network-wide DD episode" was the INSTRUMENT'S REFERENCE SATELLITE (2026-09-06)

Opened as the investigation card §6.11 items 2/6/7 pointed at ("what re-realizes NETWORK-WIDE at a
re-admission cluster that the exactly-conditioned F3 block resists").  Closed on the existing 24 h arm's
own trace without a new run: nothing re-realizes in the estimator — the DD trace's reference satellite does.

### 6.12.1 The instrument

`network_dd_fractional.csv` (filter/NetworkDdFrac.cpp) forms, per (constellation, signal), the
between-satellite SD of every graded satellite against ONE reference — "the lexically-lowest satellite
present at ≥ 2 stations" — and then the between-station DD of that SD over every station pair that sees
both.  On the wu93 24 h fixture that reference is **G01 and E02 for the entire day** (the coverage audit,
`scratchpad/ddep/dd_coverage.py`: G01 on 22 of 24 hours, G02 substituting only while G01 is excluded at
h4–5 and h16–17; E02 on 22 of 24 hours, E03 at h3–4 and h17–18).  Two consequences the graders never
controlled for:

1. **Coverage rotates with the reference's footprint.**  A chord exists only for station pairs that both see
   G01 (E02), so the trace covers 21–41 of the 60 stations at any hour (196–437 pairs) and the covered set
   is a different region hour by hour (S02–S36 at h15, S20–S60 at h19).  Every hourly, clean/touched,
   mature/young and per-station split of §6.11 compared DIFFERENT station populations.
2. **Every chord inherits the reference's own error.**  With s the between-station SD ambiguity error of a
   satellite at a pair, the graded quantity is e_i = s_i − s_ref: the reference's error is COMMON to all
   chords of a (ref, sta_a, sta_b, signal) group.  When G01 / E02 return from eclipse exclusion (G01 at
   h5.6 and h17.6, E02 at h4.3 and h18.4 — the GPS and Galileo orbital periods), the whole trace is
   re-referenced to a satellite whose legs were just reborn at every station, and the trace reads that
   satellite's per-station convergence as a network-wide episode.  The "episodes" at h6–h9 and h18–h22 are
   the hours after those four returns, and h14–h15, where G12 (14.3) and E25 (16.6) returned but the
   references did not, show none — the cluster hypothesis never fit that.

### 6.12.2 The decisive test — re-referencing in post-processing

Because the reference's share is common to the group it cancels in any between-satellite difference.
`scripts/simnet_dd_reref.py` (selftest `--selftest`) streams the trace and reports, per hour and
constellation, the as-traced grade med|e_i|, the group's COMMON component c = median_i e_i (≈ −s_ref when
most legs are good), the REF-FREE residual med|e_i − c| with its round-to-zero share (groups of ≥ 3 sats),
and an oldest-reference variant e_i − e_j with j the longest-present satellite.  Base F3 arm
`runs/nonint_losblock24h_hv`, L1C, stride 4:

| hour | ref | as-traced med\|e\| (round0 %) | common med\|c\| | ref-free med\|u\| (round0 %) |
|---|---|---|---|---|
| 2 (quiet) | G01 | 0.173 (83.8) | 0.128 | 0.094 (91.8) |
| 7 (episode 1) | G01 | **0.523 (48.9)** | **0.515** | 0.110 (83.3) |
| 13 (quiet) | G01 | 0.166 (86.7) | 0.119 | 0.078 (93.6) |
| 19 (episode 2) | G01 | **0.592 (46.9)** | **0.593** | 0.105 (85.4) |
| 20 | G01 | 0.485 (50.4) | 0.505 | 0.087 (86.5) |
| Galileo 2 / 6 / 20 | E02 | 0.217 / **0.486** / **0.659** | 0.176 / 0.526 / 0.602 | 0.087 / 0.084 / 0.108 |

The common component tracks the as-traced number to the digit through both episodes; the ref-free residual
is **flat all day at 0.07–0.11 cyc L1C (83–94 % at the integer)**, WL(1,2) 98.6–99.6 % and EWL(2,5) 99.7–100 %
ref-free in every hour.  Split by the graded satellite's OWN leg age (epochs of continuous presence at the
pair; `dd_reref_split.py`): young (< 120 ep) 0.08–0.17 with a mild rise in the episode hours (the
returning satellites' own legs), mid (120–360) 0.045–0.085 and mature (≥ 360) 0.015–0.079 with NO episode.
The ref-free convergence curve of a leg is 0.270 / 0.170 / 0.114 / 0.066 / 0.046 cyc at 1–10 / 10–30 /
30–100 / 100–300 / 300–1000 epochs.

### 6.12.3 Corrected readings of the §6.11 arms (ref-free, `simnet_dd_reref.py`, stride 8)

- **Arm 2 (`readmit24h_e2`)**: identical to the base in BOTH frames (ref-free h7 0.105 / h19 0.102 vs base
  0.110 / 0.105) — the coupling window and the seeds never had a DD target.  `readmit_coupling_marg_ep`
  KEEP OFF stands, for the corrected reason.
- **Arm 5 (`readmit8h_young`, diagonal young rows)**: ref-free 1.14–2.66 cyc every hour — the harm was
  REAL in every frame (§3.1's station-common over-count re-entering through the diagonal rows).  Verdict
  unchanged.
- **Arm 5b (`readmit8h_youngfree`)**: ref-free h2–h7 0.113 / 0.093 / 0.084 / 0.086 / 0.122 / 0.119 vs base
  0.088 / 0.089 / 0.076 / 0.074 / 0.088 / 0.094 — a small COST (the free young legs at 1.7–2.0 cyc pull the
  group medians), no benefit.  KEEP OFF stands.
- **The 24 h "settled quarter 0.420 cyc / 53 %" (§6.11 item 1 and every place it is quoted)** is the
  reference's convergence after its h17.6 / h18.4 returns.  Full-trace `--ref-free` summary (1,523,679 groups of
  ≥ 3 sats graded, 741,312 pairs with < 3 sats skipped): L1C med|u| **0.089 cyc / 86.8 % at the integer** over
  the day and **0.095 / 85.9 %** over the last quarter (as-traced 0.283 / 64.2 % and 0.425 / 53.2 %); L2W
  0.120 / 84.7 %, L5Q 0.117 / 83.5 %, L7Q 0.100 / 82.7 %; EWL(2,5) 0.012 cyc / 100 %, WL(1,2) 0.029 / 98.8 %
  (last quarter 99.1 %).  The by-age slopes (−0.46) are also contaminated: a chord's "age" restarts when the
  reference returns while its own leg is old.

### 6.12.4 What the field inherits

The DOY-129 fractional histogram (`med|frac_cyc|` uniform 0.25, SPEC_pivot / ledger §A2) rides the same
trace and the same reference rule; a young or slipping G01 smears every fraction of the constellation.
That verdict's CONCLUSION (no integer structure) does not rest on the reference alone, but every fractional
number from that trace must be re-read knowing that one satellite's fraction is in all of them.
Consumers of the trace that inherit the rule: `scripts/grade_ddfrac.py`, `grade_ddfrac_perhour.py`,
`grade_churn_arms.py`, `field_lane_frac.py` and the pyviz viewer `network_dd_fractional.py` — none of them
re-references; each reports the reference's fraction folded into every chord.

### 6.12.5 Fixes

1. **Grader — DONE.**  `scripts/simnet_amb_integer.py --ref-free` adds a REF-FREE block (per band + lanes,
   all / last-window; default output byte-identical; selftest extended with a bed whose reference carries a
   known error) and `scripts/simnet_dd_reref.py` is the hourly per-constellation instrument with the
   leg-age curve.  Quote DD grades ref-free from now on.
2. **Producer — OWED (design; blocked 2026-09-06 only because the concurrent session holds uncommitted edits
   in `GnssConfig.hpp`, `GnssConfigTypes.hpp` and `NetworkRunContext.*`).**  `network.trace_dd_fractional_ref`
   = `"lowest"` (default, byte-identical) | `"oldest"`: per (constellation, signal) keep in `NetworkCarryState`
   an age map (epochs of continuous presence at ≥ 2 stations, reset on absence); pick the satellite with the
   largest age, ties → lexically lowest; count reference switches and print them at run end; manifest key;
   posture row SCOPED under `trace_dd_fractional`.  Trace-class, fingerprint-SAFE.  Validation = an 8 h
   same-build A/B (`lowest` vs `oldest`): products byte-identical, and the `oldest` arm's AS-TRACED hourly
   grade must match the base's REF-FREE grade within the leg-age noise.  The ref-free grader remains the
   reference-independent instrument even after the knob lands (a single old reference still carries its own
   slip / re-level history).
3. **Method rule (also SPEC_synthetic §13.8):** a grade built on a SINGLE reference element must report that
   element's share separately or difference it away; "the whole network moved at once" is the signature of a
   moved reference, and the first check on any network-wide episode is which reference the instrument used
   and what happened to it at the onset.

Result: the card's target does not exist; the re-admission edge card's remaining DD item is closed as an
instrument artifact and the young-leg convergence rate (6.12.2's curve) is the only per-satellite residual.

### 6.13 The FIELD re-grade, and the producer knob (2026-09-06)

§6.12.4 left the DOY-129 fractional histogram owed a re-read: it rides the same trace and the same
reference rule, so "one satellite's fraction is in all of them" applies there too.  It is now measured, and
**the answer is that the field's uniform-0.25 histogram is NOT this artifact.**

#### 6.13.1 A truth-free instrument, and why it needs a null

The field has no `truth_ambiguities.csv`, so `simnet_dd_reref.py` does not apply.  The only observable is the
trace's own `frac_cyc`, which lives on the unit circle.  That is enough: within one
(epoch, cons, signal, sta_a, sta_b) group the reference's share is COMMON, so a circular median estimates it
and the residual is per-satellite.  `scripts/dd_frac_reref.py` (selftest `--selftest`, two beds - one
reference-dominated, one pure uniform noise) reports four readouts per hour and constellation.

⚠ **Every one of them needs a null, and two of the four nulls are counter-intuitive.**

1. **The ref-free residual shrinks by chance.**  Subtracting a group's OWN circular median from its members
   reduces |residual| even for independent uniform fractions - a group of 4 has four chances to sit near its
   own median.  Measured on the uniform bed: as-traced 0.251 -> ref-free **0.145**, a 42 % "improvement" that
   is pure estimator artifact.  The tool therefore prints a size-matched Monte-Carlo null beside every
   ref-free number and the verdict is the **GAP**, never the raw value.
2. **The triangle-closure null is NOT independent-uniform.**  A DD is per-station additive by construction
   - f_i(a,b) = [s_i(a) - s_i(b)] - [s_ref(a) - s_ref(b)] - so for a FIXED satellite the triangle closes
   exactly, reference or no reference; and because c-hat is a circular median it IS one of those data points,
   so a triangle closes whenever the median lands on the same satellite in all three pairs.  Against an
   independent-uniform null the field "closed" at 0.182 vs 0.250 and read as real structure.  Against the
   correct null - the DD structure with NO reference term - it is 0.182 vs **0.172**, i.e. exactly at the
   null.  The first null would have manufactured a finding.

The resultant length R = |mean exp(2*pi*i*f)| is the readout with no estimator in it at all, and it carries
the verdict.

#### 6.13.2 The instrument fires on the sim and is silent on the field

Same tool, same settings; the sim row is the §6.12 base arm, where the artifact is independently known from
the truth grade.

| | sim base `nonint_losblock24h_hv` (G) | field `fld63_stk_on` L1C | L2W | L1W |
|---|---|---|---|---|
| as-traced med\|f\| (uniform null) | **0.168** (0.248) | 0.251 (0.246) | 0.251 (0.247) | 0.248 (0.251) |
| near-integer share (\|f\| <= 0.1) | **32.3 %** | 19.5 % | 20.0 % | 20.0 % |
| resultant R (null) | **0.610** (0.430) | 0.468 (0.473) | 0.447 (0.452) | 0.441 (0.438) |
| ref-free med\|u\| (null), GAP | 0.079 (0.126) **-0.047** | 0.117 (0.109) +0.008 | 0.127 (0.117) +0.010 | 0.130 (0.123) +0.006 |
| triangle closure (DD-structure null) | **0.088** (0.184) | 0.182 (0.172) | 0.182 (0.174) | 0.175 (0.171) |

The truth-free ref-free number on the sim (**0.079**) reproduces the truth-graded one (**0.089**, §6.12.2), so
the instrument is validated on real data and not merely on its own bed.  On the field **every readout sits at
its null**: the wrapped fractions are uniform WITHIN each group, so there is no common component to remove,
and no reference policy - `lowest`, `oldest`, or an oracle - can improve the field histogram.

Two supporting facts, both from the same run.  The field reference **already rotates on its own**: 8-11
switches over 12 h across 3-9 distinct satellites (G01 dominant, G03 / G06 substituting), against the sim's
G01/E02 all day.  And the as-traced grade does **not** degrade after a switch - med|f| 0.246 in the 20 epochs
following a switch vs 0.251 settled - which is the field analogue of the sim's episode signature, absent.

**Verdict: the DOY-129 "no integer structure" reading stands on its own evidence.**  It was never resting on
the reference.  §6.12.4's caution was right to raise the question and is now answered: field DD fractions are
uniform per-satellite, not uniform-because-smeared-by-one-satellite.  What the field DOES inherit from §6.12
is the COVERAGE half - a chord exists only for pairs that both see the elected reference - so per-station and
per-region field splits off this trace still compare rotating populations.

⚠ **A run-selection lesson, paid for in this session.**  The first two "sim controls" chosen (`camp60wu_f3`,
`camp60wu_quiet24h_sticky`) showed field-like uniformity and appeared to contradict §6.12 outright.  They are
different arms whose DD errors are 2-20 CYCLES, so their fractions wrap into uniformity and the reference's
share is unrecoverable from the wrapped file.  **A truth-free instrument is only meaningful where the
underlying errors are sub-cycle**; check the truth-graded magnitude before reading a wrapped grade, and pick
the control arm by name from the document that made the claim, not by resemblance.

#### 6.13.3 The producer knob - BUILT, not yet validated

`network.trace_dd_fractional_ref` = `"lowest"` (default) | `"oldest"`, per §6.12.5's design.

- `include/config/GnssConfigTypes.hpp` - `enum class NetworkDdFracRef` + the `NetworkConfig` field.
- `src/core/config/GnssConfigLoadNetwork.cpp` - parse; an unrecognised name warns and falls back (trace-only,
  so a typo must not kill a long run, but it must never pass silently).
- `include/app/NetworkRunContext.hpp` - `ddfrac_ref_age` / `ddfrac_ref_prev` in `NetworkCarryState`, and the
  three counters.  Age = consecutive TRACED epochs at >= 2 stations; **an absence ERASES the entry**, so a
  returning satellite re-enters at age 1 and cannot inherit its pre-gap seniority - which is the point, since
  a returned reference's newborn legs are exactly what re-referenced the trace.  Not snapshotted: a warm
  resume restarts ages at zero and re-elects by the tie-break, and the run-end switch count makes that visible.
- `src/core/filter/NetworkDdFrac.cpp` - the election.  `satmap` is a `std::map`, so the scan is already
  lexical and a STRICT `>` keeps the lowest of a tied age: **`oldest` degenerates to `lowest` exactly when
  every candidate is the same age**, which is what makes the age_win counter meaningful.
- `src/core/filter/NetworkTeardown.cpp` - run-end readout on BOTH policies.  The switch rate was always there
  and always silent; a grade built on a single reference element has to state that element's behaviour
  (SPEC_synthetic §13.8).

**Status: A/B RUN 2026-09-06 — the mechanism WORKS, the pre-registered PASS criterion FAILS, and the
goldens still stay on `lowest`.**

6 h same-build arms `_ddref6h_low` / `_ddref6h_old` on the F3 base lineage, single knob, crossing G01's h4-5
and E02's h4.3 exclusions.

*Structural half — PASS.*  `products.txt` byte-identical (and the `lowest` arm was re-run from scratch and
reproduced its own hash, so the pipeline is deterministic); `network_solution`, `network_events`,
`network_epoch_status`, `network_stations` byte-identical; `network_dd_fractional.csv` differs, as it must.
NOT inert: 4320 elections each, **765 elections differed from `lowest`**, reference switches **12 -> 6**, and
the reference census shifts hard toward the settled satellite (E03 36,381 -> 112,394 L1C rows;
G02 56,050 -> 81,542).

*Grade half.*  h0-h3 and GPS h4 are IDENTICAL arms — early in a run every satellite has the same age, so the
tie-break returns the lexically lowest, and h0 is ignition (as-traced 1.25-1.32 cyc in both) and not gradeable
at all.  The divergent hours, truth-graded L1C, stride 4:

| | `lowest` as-traced | `oldest` as-traced | base REF-FREE |
|---|---|---|---|
| E h4 | 0.280 cyc / 69.8 % (E02+E03) | **0.227 / 73.6 %** (E03) | 0.068 / 89.0 % |
| E h5 | 0.334 / 58.9 % (E02) | **0.195 / 70.3 %** (E03) | 0.058 / 87.5 % |
| G h5 | 0.236 / 74.6 % (G02+G01) | **0.170 / 84.6 %** (G02) | 0.081 / 91.6 % |

So `oldest` does exactly what it was built to do: at the hours where the lexically-lowest reference has just
returned from eclipse, it elects the satellite that stayed up and the as-traced grade improves 25-42 % (E h5
0.334 -> 0.195, and 58.9 -> 70.3 % at the integer).  **But it does NOT reach the ref-free grade — it lands
2.1-3.4x above it** — so §6.12.5's pre-registered PASS ("the `oldest` arm's as-traced grade matches the base's
ref-free grade within the leg-age noise") is **FAILED**, and it was the wrong bar: it assumed a settled
reference contributes negligibly, and the measurement says a settled reference still contributes 2-3x the
ref-free residual.  That is the theory working, not a defect — a single reference of ANY age is still one
satellite's error in every chord.

Two second-order readings, both coverage effects rather than quality regressions.  The `oldest` arm's own
REF-FREE grade is slightly worse at the divergent hours (E h4/h5 0.095 / 0.111 vs the base's 0.068 / 0.058)
and its leg-age curve starts higher (0.355 vs 0.267 cyc at 1-10 epochs), because electing a different
reference changes WHICH station pairs have chords at all (E h4: 7,307 graded vs 8,388) and E03's footprint
skews younger.  **A policy A/B moves the covered population, so never compare the two arms' ref-free numbers
as if they were the same chords** — the as-traced column is the one the knob is about.

Ledger verdict: **RECOMMEND_SCOPED**, narrowly.  The goldens keep `lowest` under a documented manifest
exception, for a reason that outranks the diagnostic gain: the goldens are the comparison baseline, and
changing which satellite their dd-frac trace references breaks continuity with the entire existing record of
dd-frac grades.  There is no loader posture row — this is an option, not a recommendation the loader should
push.

And the caveat the knob's name invites getting wrong, now measured rather than argued: **an `oldest` trace is
quieter, never reference-FREE.**  For any GRADE the fix is `simnet_dd_reref.py` / `--ref-free`; the knob is for
the case where the as-traced CSV must stand alone — a viewer, an archived file, a consumer script that does
not re-reference.

#### 6.13.4 ⚠ A/B HYGIENE — the stale-trace trap that faked byte-identity here

The first pass of this A/B reported "byte-identical on every trace including `network_dd_fractional.csv`".
**That was wrong, and the way it was wrong is worth more than the result.**  The network traces are written
CWD-relative to `./logs/`, but the capture script (adapted from `scripts/_arm_run.sh`) copied from
`build/x64-Release/logs/`, which held a **stale 10-epoch file from 2026-08-13**.  Both arms copied the same
old file, so the comparison was stale-vs-stale and returned IDENTICAL for every trace — the single most
reassuring possible answer, produced by reading nothing.  It survived a smoke A/B and was only caught because
the run-end counter said 765 elections differed while the files said none did.

Three rules, now enforced in the capture script:

1. **Assert the captured file is NEWER than the run's start time.**  Presence is not freshness; a copy's own
   mtime is always new, so `stat` the SOURCE, not the destination.
2. **Capture from `./logs/` and verify the path on a file whose content you can date** (a row count, an epoch
   span).  A trace that is the wrong size by two orders of magnitude is the cheapest possible check.
3. **A trace-only A/B must be cross-checked against a run-end COUNTER.**  Byte-identity is the expected result
   of a correct OFF path AND of a broken capture; only a counter distinguishes them (standing invariant #4
   applied to the instrument itself).

The second arm also OVERWROTE the first arm's `./logs/` trace before it was captured — the concurrent-run log
collision, hit sequentially — so the `lowest` arm had to be re-run.  `scripts/_arm_run.sh` carried the same
wrong source path and is fixed: it reads `./logs/` and asserts the SOURCE mtime is newer than the run's start.

**The smoke A/B was then re-run through the fixed capture (2026-09-06, Release rebuilt from committed source,
sha256 `fe907a95…`), and it IS byte-identical — genuinely this time.**  `products.txt` and every trace
(`network_dd_fractional`, `_solution`, `_events`, `_epoch_status`, `_estimability`) match, with no `STALE`
warning on either arm.  The two cheap checks that would have caught the original failure both fire correctly
now: the captured trace is **111,139 rows over 40 epochs** where the stale file was 20,001 rows over 10 — a
5.5x size and 4x epoch-span discrepancy visible without opening it — and the counter cross-check AGREES with
the files (`0 election(s) differed from 'lowest'`, 240 elections per arm, 0 switches).  That agreement is the
whole point: byte-identity plus a zero counter is a consistent, explained result, whereas byte-identity plus
`765 elections differed` was the contradiction that exposed the broken capture.  A trace-only A/B is only
believable when both readings are quoted together.

