# COLD-FB (iter 2–4) — the SATELLITE clause, the datum-invariant consumer census, and the new-golden arm — KEEP_OFF on all four iterations

**STATUS**

- **id:** COLD-FB (iter 2–4)
- **topic:** network-vehicle
- **status:** parked
- **summary:** the SATELLITE clause, the datum-invariant consumer census, and the new-golden arm — KEEP_OFF on all four iterations
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.phase_datum_candidacy_satellite`, `network.phase_datum_candidacy_usable`
- **hashes named in the text** (commit / exe / byte-cert): `1b80ce25`, `e90086f`
- **run dirs named in the text:** `runs/golden_sim_campaign_zwdblk_coldfb3`, `runs/golden_sim_campaign_zwdblk_scoped`
- **source:** `docs/archive/SPEC_event_harness.md` — 15.8, 15.9, 15.10, 15.11

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 15.8 ITERATION 2 — the SATELLITE clause: it removes every split, it fixes the slip leg, and on the storm arm it REFUTES the reading that motivated it

`network.phase_datum_candidacy_satellite` (bool, default **TRUE** under
`phase_datum_candidacy_usable`; OFF reproduces iteration 1 exactly, which is what
makes the A/B a single knob and what makes the two storm arms differ only in the
code default).  Built on the §15.7 reading: the per-family screen is right about
WHICH carry is usable and wrong about WHOSE satellite it belongs to.

**The rule** (locked before any leg).  When the candidacy pass is armed:

* **(a) JOINT resolution** — every family of satellite *s* handing over THIS epoch
  takes ONE successor: the first coverage-ranked candidate usable for the LARGEST
  number of them, ties to coverage order.  A family that host is not usable for is
  still pinned THERE, through the honest cold path (`sat_clause_cold_colocated`):
  a cold pin on the same host is a frame step, not a split.  No candidate usable
  for any mover ⇒ today's sweeps unchanged, counted.
* **(b) PARTIAL handover** — when only some of *s*'s families move, the station
  already anchoring the others is tried FIRST and, if it tracks and is usable for
  at least one mover, takes them all (`sat_clause_colocated`).  ⚠ *Reading, not
  transcription:* taking the held host on "usable for ≥ 1" rather than only on a
  joint-maximum tie is the only reading under which (c) can hold — letting a
  higher-scoring stranger win re-creates exactly the split this iteration removes,
  and a family the held host cannot serve takes a co-located cold pin instead.
* **(c) the census** — the run-end clause reports the per-satellite distinct-anchor
  numbers from the SAME always-on counters the storm grade reads
  (`networkSplitReferencedSats`), so the clause and the grade agree by
  construction rather than by agreement.

One acceptance lambda now has three readers (the per-family pass, the joint
scoring, the co-located cold decision).  The per-family pass still runs under the
clause — it owns the four skip counters and the row's `cand_skipped` /
`cand_first_unusable`, and it is the FAMILY-ONLY pick `sat_clause_split_avoided`
is measured against.  A family the joint host does not TRACK cannot be pinned
there and falls back per-family: the only residual split, counted and warned.
Tests: +7 cases in the same two suites (24 total), each asserting the
per-satellite distinct-station count; full Debug suite **1924 / 261 suites, 1907
passed, 17 skipped, 0 failures**.

**Harness** (quiet seed ep0665, 60-epoch legs, ON arms at `min_age_ep` 0 for the
§15.4 warm-resume reason, which is unchanged).

| leg | exit / wall | split-ref mean / max | handovers / continuous (cold) | consumer +40 | consumer end | ZWD pooled rms |
|---|---|---|---|---|---|---|
| `null_off` | 0 / 188.0 s | 0.0 / 0 | 24 / 21 (**3**) | 1.7108 | 1.5464 | 0.07211 |
| `null_on0` (iter 1) | 0 / 144.1 s | 0.0 / 0 | 24 / 24 (0) | 1.6645 | 1.4904 | 0.07180 |
| `null_on2` (iter 2) | 0 / 151.5 s | 0.0 / 0 | 24 / 24 (0) | 1.6645 | 1.4904 | 0.07180 |
| `g30_off` / `g30_on0` / `g30_on2` | 0 / 174.3, 182.6, 145.5 s | 0.0 / 0 | 24 / 24 (0) | 1.8664 (all three) | 1.6769 (all three) | — |
| `slip_off` (§12.7, 40 cyc L1C at S29000SIM/G10) | 0 / 138.1 s | **4.3 / 6** | 41 / 38 (**3**) | 1.1132 | 1.0614 | 0.07262 |
| `slip_on` (iter 1) | 0 / 142.5 s | **6.0 / 7** | 39 / 39 (0) | 1.2412 | **1.1718** (+110 mm) | 0.07184 |
| `slip_on2` (iter 2) | 0 / 151.1 s | **4.3 / 6** (= OFF) | 38 / 38 (0) | 1.0046 | **1.0240** (−37 mm vs OFF) | 0.07209 |

`null_on2` and `g30_on2` are **byte-identical** to their iteration-1 twins
(products, DD and solution all match) — those windows had no split to avoid, and
the clause's own counters say so (`sat_clause_split_avoided` = 0 there).  The slip
leg is the one that moved, and it is the leg iteration 1 hurt: 16 satellite-epochs
resolved jointly over 38 families, **2 by the held-host preference and 3 splits
avoided**, `handover_cold` still 3 → 0.

**The attribution is clean.**  On the slip leg the emitted G10 wire is essentially
the same in both iterations (clock Δ −0.0007 m, L1C Δ −0.039 m, L2W Δ −0.046 m at
the leg end), while the consumer difference against OFF flips **+0.110 m → −0.037
m**.  The consumer gain is therefore NOT the injected family's own wire — it is
the satellites whose references iteration 1 split and iteration 2 did not.  That
is the §14.7 channel, measured directly: an inter-frequency inconsistency the
per-band producer instruments cannot see and the user pays for.

**G3 storm arm `coldfb2`** (`config/test_suite/_golden_sim_campaign_zwdblk_coldfb2.json`
= the `coldfb` arm on iteration-2 code, where the clause is the code default, so the two
arms differ by exactly that; exe `gnss_processor_coldfb2_storm.exe` = `1b80ce25…`,
720/720, exit 0, 1406 s).  **B** = iteration 1 (`coldfb`), **B2** = this arm.

| channel | A (OFF) | C5 (cbscope5) | B (iter 1) | **B2 (iter 2)** | pre-registered bar | verdict |
|---|---|---|---|---|---|---|
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 0.0 / 0 | 0.9 / 3 | **0.0 / 0** | 0.0 / 0 | **PASS** |
| PHASE_ANCHOR handovers / cold | 305 / 68 | 315 / 78 | 309 / 46 | 312 / **46** | ≤ 46 | **PASS** |
| ...cold-start / settled | — | 27 / 51 | 27 / 19 | **27 / 19** | settled ≤ 19 | **PASS** |
| ...cold by hour h0…h5 | 27 11 7 2 8 13 | 41 12 7 0 5 13 | 38 8 0 0 0 0 | **38 8 0 0 0 0** | | zero from hour 2 |
| PB-GAUGE elected / continuous / cold re-realizations | — | 462/362/100 | 473/421/52 | 468/416/**52** | ≤ 52 | **PASS** |
| continuous successor arc age p10 / p50 / p90 | — | 91 / 240 / 459 | 99 / 231 / 475 | **116 / 236 / 478** | p10 ≥ 60 | **PASS** |
| candidacy entered / usable-elected / rank-changed / fell through | — | — | 473/404/53/69 | 468/399/**60**/69 | | |
| SATELLITE clause: sat-epochs / families / co-located / **splits avoided** / cold co-located / untracked / no-usable | — | — | — | 133 / 399 / 0 / **13** / 0 / 0 / 23 | | the clause fired |
| **consumer settled p50 (pooled) / rms** | .1741 / .365 | .1204 / .2215 | .1794 / .2823 | **.1941 / .3091** | ≤ .1204 | **FAIL** |
| W1 window-excluded p50 | .1663 | .1028 | .1456 | **.1751** | ≤ .1028 | **FAIL** |
| consumer hourly p50 h0…h5 | .447 .247 .229 .174 .086 .280 | .524 .113 .114 .101 .090 .236 | .610 .263 .240 .186 .109 .204 | **.610 .264 .243 .189 .110 .232** | h1–h3 within +2 cm of C5 | **FAIL** (+15 / +13 / +9 cm) |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .65 .11 .12 .10 .24 .28 | .69 .26 .236 .18 .327 .315 | **.69 .26 .24 .182 .31 .397** | | |
| pivot window 4.68–5.40 / last 36 min | .273 / .403 | .221 / .267 | .314 / .160 | **.367 / .219** | | |
| **ZWD settled truth rms / span p50** | .0071 / .031 | .0069 / .021 | .0061 / .0199 | **.0061 / .0197** | ≤ .0069 | **PASS — best, tied with B** |
| **ref-free WL / EWL / L1C** | 98.9/100/87.1 | 99.2/100/89.9 | 99.3/100/90.9 | **99.3/100/90.8** | ≥ 99.2 / ≥ 89.9 | **PASS** |
| T4 events / ambient | 78 / .229 | 11 / .197 | 11 / .196 | **11 / .196** | ≤ 11 | **PASS** |
| W2 4.9917 h T4 / decouples / declared CLK breaks | 0/0/0 | 0/0/0 | 0/0/0 | **0/0/0** | absent | **PASS** |
| G4 unanswered-exonerate / spurious-implicate | — | 0 / 2 | 0 / 2 | **0 / 2** | 0 / (residual) | **PASS** |
| anchor stations holding the datum / epoch (GPS / Gal) | — | 17.26 / 11.42 | 17.76 / 11.63 | **17.16 / 11.56** | | back to C5's spread |
| run time | 1130 s | 1050 s | 1226 s | **1406 s** | | +34 % vs C5 |

**The clause did its job and the consumer did not follow.**  Split-referenced
satellites are **0.0 / 0 all run** — the primary pre-registered gate, met exactly, with
13 splits avoided and the datum spread back to C5's (17.16 vs 17.26 GPS stations per
epoch).  Every producer gate passes and three are the best of any arm.  And the
consumer is **worse than iteration 1** (pooled p50 .1794 → .1941), not better.

**So the §15.7 reading is FALSIFIED: the split reference was a correlate, not the
mechanism.**  Removing every split moved the consumer the wrong way by 1.5 cm.

**What the consumer difference actually is — measured, and it is a grading trap.**
Decomposing the settled consumer error into its mean ENU components:

| arm | Ē (m) | N̄ (m) | Ū (m) | \|U\| p50 | 3D p50 |
|---|---|---|---|---|---|
| A (scope OFF) | **+0.0771** | +0.0309 | −0.1615 | 0.1186 | 0.1741 |
| C5 (cbscope5) | **+0.0381** | −0.0255 | −0.0437 | 0.0739 | 0.1204 |
| B (iter 1) | **+0.0776** | −0.0295 | −0.0072 | 0.1036 | 0.1794 |
| B2 (iter 2) | **+0.0786** | −0.0452 | −0.0411 | 0.1124 | 0.1941 |

The EAST bias is +7.7 to +7.9 cm on **A, B and B2 alike** and +3.8 cm on C5 — C5 is the
outlier, not the candidacy arms.  The static bias-vector magnitude is 6.3 cm for C5 and
9.9 cm for B2, which is ~3.6 cm of the 7.4 cm median gap; and the vertical, which
iteration 1 disturbed (Ū −0.007 against C5's −0.044), iteration 2 puts back (−0.041).
Against the scope-OFF baseline the card costs **+0.5 cm (B) / +2.0 cm (B2)** on the 3D
median, not the +49 % the C5 comparison reads.

That does not rescue the gates — they were set against C5 and they fail — but it names
what the failure is measuring: **a single-realization horizontal datum bias that differs
between C5 and every other arm on this bed, including the arm the card was built on.**
Standing invariant #5 says exactly this about consumer position as a grading channel, and
the correct instrument here is a datum-invariant one.

### 15.9 Verdict on iteration 2, and what iteration 3 must test

**`network.phase_datum_candidacy_usable` stays KEEP_OFF**, now with both iterations
merged default-OFF and the satellite clause default-ON beneath it.  The pre-registered
gates: **11 pass** (split 0.0/0, cold 46 total and 19 settled, PB-GAUGE cold 52,
successor age p10 116, ZWD 0.0061, WL 99.3, L1C 90.8, T4 11, W2 absent, G4 unchanged) and
**3 fail**, all three consumer: pooled p50 .1941 vs ≤ .1204, window-excluded .1751 vs
≤ .1028, hourly h1–h3 +9 to +15 cm vs the +2 cm allowance.

What iteration 2 SETTLED, and it is worth the run on its own:

1. **The split reference is not the consumer cost.**  Zero splits, consumer 1.5 cm
   worse.  The §15.7 mechanism is refuted by its own fix.
2. **The clause is nonetheless right and cheap** — it costs 13 election changes in 6 h,
   restores the datum spread, restores the vertical, and keeps every producer gain.  It
   is the form the knob should keep if it is ever armed.
3. **The consumer comparison against cbscope5 is confounded** by a 4 cm east-bias
   difference that the candidacy arms share with the scope-OFF baseline.

**Iteration 3, pre-registered before it exists.**  The open question is no longer "which
station" but "what does moving the datum host at all cost the user", and the C5
comparison cannot answer it because C5's own realization differs.  Two things, in order:

⚠ **§15.11 reports iteration 4 on the NEW golden (scoped_handover_response promoted): 12 of 15 gates pass — every wire instrument beats the new reference — and the de-biased consumer misses by 4 mm while one pivot edge regresses.  KEEP_OFF stands.**

⚠ **§15.10 reports iteration 3.  The first item below is DONE and it inverted the A-comparison: per-hour-de-biased, B2 is 47 % BETTER than the scope-OFF arm, not worse — but cbscope5's advantage SURVIVES de-biasing, so the §15.8 gate failures stand.**

* **A datum-invariant consumer instrument.**  Re-grade every arm on a quantity the
  station's own coordinate bias cannot enter — the between-satellite emitted-bias
  consistency, or the consumer's ambiguity-fixing / residual channel — and re-run the
  §15.6 and §15.8 verdicts against it.  Until that exists, no consumer gate on this bed
  distinguishes a card from a realization.
* **The COVERAGE cost of a usable-carry host.**  The pass by construction elects a
  later-ranked station (60 rank changes here).  Measure the elected successors' coverage
  rank distribution and test the cheapest restraint: accept a later-ranked candidate only
  while its coverage is within k of the top-ranked one's (k a knob, 0 = today), so the
  card buys carry continuity without ever moving the datum to a thin host.  Gate: the
  producer channels of B2 kept, and the consumer within the A-arm's own spread.

### 15.10 ITERATION 3 — the DATUM-INVARIANT consumer census: the raw median INVERTS the ranking, and the wire instruments are the ones that move with the cold count (no runs; `scripts/ev_consumer_invariant_grade.py` over the four existing storm arms)

§15.8 ended on a suspicion: the consumer gates were read against `cbscope5`, whose
settled mean EAST error is +3.8 cm while the scope-OFF baseline and BOTH candidacy
arms sit at +7.7…+7.9 cm.  Iteration 3 is the instrument that settles it — no new
runs, the four arms already on disk (A = `mwevid`, C5 = `cbscope5`, B = `coldfb`,
B2 = `coldfb2`), graded on quantities a static station-coordinate bias cannot enter.

**The instruments** (all over the settled window h ≥ 3 unless stated):

* **bias** — the settled mean ENU and its magnitude, reported so the confound is
  visible instead of hiding inside every other number.
* **debias / debiasH** — `|error − the arm's OWN settled mean ENU|`, p50 and rms;
  and the same with a **per-hour** mean removed, so a slowly MOVING bias cannot
  hide inside the static removal.  If the two disagree, the bias is drifting.
* **consumer re-convergence** — from the consumer's own `logs/rt_engine.log`: SSR
  phase-bias resets, code-bias discontinuities, clock discontinuities.  This is
  what a producer datum re-realization COSTS the user, and it is position-free.
* **shocks** — epochs after the first hour with 3D error > 0.5 m.  An 8 cm bias
  cannot manufacture one.
* **wire** — the emitted-bias step series from `products.txt`: every (sat, signal)
  phase bias against the previous emitted epoch, summarised as the fraction of
  epochs carrying ANY |step| > 0.05 m and the p90 of the per-epoch maximum.  A
  CONTINUOUS handover is an S-transform and makes no step; a COLD one does.

| arm | cold handovers | Ē | N̄ | Ū | \|bias\| | raw p50 / rms | **debias p50 / rms** | **debiasH p50 / rms** |
|---|---|---|---|---|---|---|---|---|
| A (scope OFF) | 68 | +0.0771 | +0.0309 | −0.1615 | 0.1816 | 0.1741 / 0.365 | 0.1323 / 0.317 | **0.1532 / 0.309** |
| C5 (cbscope5) | 78 | +0.0381 | −0.0255 | −0.0437 | **0.0633** | **0.1204 / 0.222** | **0.0646 / 0.212** | **0.0521 / 0.206** |
| B (iter 1) | 46 | +0.0776 | −0.0295 | −0.0072 | 0.0833 | 0.1794 / 0.282 | 0.1363 / 0.270 | **0.0943 / 0.256** |
| B2 (iter 2) | 46 | +0.0786 | −0.0452 | −0.0411 | 0.0996 | 0.1941 / 0.309 | 0.1150 / 0.293 | **0.0815 / 0.284** |

| arm | PB resets | CB discs | clock discs | shocks (> 0.5 m, after h1) | AR | wire: frac epochs \|step\| > 5 cm | wire: p90 max step |
|---|---|---|---|---|---|---|---|
| A | 1867 | 599 | 40 | 46 | never ran | 0.3708 | 0.5280 m |
| C5 | **1938** | 597 | 39 | **13** | never ran | 0.3694 | 0.4000 m |
| B | **1772** | 593 | 39 | 21 | never ran | **0.3083** | **0.2682 m** |
| B2 | 1794 | 593 | 39 | 26 | never ran | **0.3056** | **0.2698 m** |

Hourly (raw p50 / per-hour-de-biased p50 / PB resets / shocks):

| arm | h0 | h1 | h2 | h3 | h4 | h5 |
|---|---|---|---|---|---|---|
| A | .447/.317/905/45 | .247/.097/3/0 | .229/.032/8/0 | .174/.054/0/0 | .086/.174/468/24 | .280/.238/483/22 |
| C5 | .524/.284/962/63 | .113/.056/0/0 | .114/.023/32/0 | .101/.031/0/0 | .090/.047/450/7 | .236/.217/494/6 |
| B | .610/.302/953/83 | .263/.052/0/0 | .240/.035/0/0 | .186/.040/0/0 | .109/.098/432/10 | .204/.205/387/11 |
| B2 | .610/.302/953/83 | .264/.054/0/0 | .243/.033/0/0 | .189/.040/0/0 | .110/.080/444/13 | .232/.239/397/13 |

**1. The raw pooled median INVERTS the ranking against the OFF baseline.**  Raw, the
order is A 0.1741 < B 0.1794 < B2 0.1941 — the card "costs" the user.  With each
arm's own per-hour mean removed the order is **B2 0.0815 < B 0.0943 < A 0.1532** —
the card is 47 % BETTER than OFF and iteration 2 better than iteration 1.  The two
readings differ because A carries a −16 cm vertical bias that the raw median
charges to nobody and the de-biased one removes.  Everything §15.6 and §15.8 said
about "the consumer" on the A-vs-card comparison was reading that.

**2. The `cbscope5` advantage SURVIVES de-biasing — plainly, and it is not the
bias.**  C5 is best of the four on every position instrument after removal (debias
p50 0.0646, debiasH 0.0521, rms 0.206–0.212).  Its favourable centring is real but
secondary: it is also genuinely the quietest arm within the hour.  So the §15.8
gate failures are NOT an artifact of C5's centring, and the honest statement is
that **B2 is better than A and worse than C5 on the position channel, both before
and after de-biasing.**

**3. The instruments that move with the cold-handover count are the WIRE ones.**
PB resets are monotone in the cold count — 78 colds → 1938 resets (C5), 68 → 1867
(A), 46 → 1772 / 1794 (B / B2) — so **B2 beats BOTH A and C5 on the consumer's own
re-convergence work**, which is the channel a cold gauge re-realization actually
acts through.  The emitted-step instrument separates card from no-card sharply:
31 % of epochs carry a > 5 cm step on B/B2 against 37 % on A and C5, and the p90
per-epoch maximum step falls 0.528 / 0.400 m → **0.268 / 0.270 m**.  That is the
card doing exactly what it was built to do, measured on the product rather than on
a station's position.

**4. The instruments that do NOT move with the cold count**: code-bias
discontinuities (599 / 597 / 593 / 593) and clock discontinuities (40 / 39 / 39 /
39) are flat across all four arms — the cold PHASE-anchor handover does not touch
them, which is a useful negative control.  Shocks are *anti*-correlated (C5 has the
most colds and the fewest shocks, 13 against B2's 26 and A's 46) and live in h0 and
h4–h5, i.e. in ignition and the §19 pivot-departure window, not in the handover
population at all.  De-biased position sits with C5, not with the cold count.

**5. What could not be computed: the AR channel and therefore the wrong-fix rate.**
`closed_loop.ar_mode = 0` on this lineage, so `ar_ran` is 0 on all 720 epochs of all
four arms, `ar_gate` reads `not_run` everywhere, `p_succ` is 0 and
`logs/ambiguity_resolver.log` is empty; there is no `ambiguity_trace.csv` for the
consumer.  The consumer is FLOAT-ONLY by configuration, so there are no fixes to
grade and no wrong fixes to count.  Re-grading that channel needs an arm with
`closed_loop.ar_mode` armed — a new run, not a new instrument.

**Verdict.**  `network.phase_datum_candidacy_usable` **stays KEEP_OFF.**  The
promotion condition was a datum-invariant channel where B2 beats BOTH A and C5 AND
no invariant channel worse.  The first half is met — PB resets (1794 vs 1867 / 1938)
and both wire-step instruments (0.306 vs 0.371 / 0.369; 0.270 m vs 0.528 / 0.400 m)
— and the second half is not: de-biased position p50 and rms and the shock count are
all worse than C5.  What iteration 3 changes is the CHARACTER of the open question.
It is no longer "does the card cost the user 49 %" (it does not; that number was a
vertical bias on the OFF arm) but "the card makes the WIRE quieter and the position
channel still prefers `cbscope5`", and the next thing to measure is why — the
candidate is the §19 pivot-departure window, where every arm's shocks and half its
PB resets live and where the cold-handover population is not.

### 15.11 ITERATION 4 — the card on the NEW golden, graded on iteration 3's invariant instruments: the wire gates pass by wide margins, the de-biased consumer misses by 4 mm, and one pivot edge regresses (2026-09-11; `runs/golden_sim_campaign_zwdblk_coldfb3`, 720/720, exit 0, 1361 s)

The scoped-handover promotion (§19.5) moved the reference: `S =
runs/golden_sim_campaign_zwdblk_scoped` is the new golden's posture on the campaign
bed, and its consumer is a different animal from cbscope5's (settled p50 0.1204 →
0.0825, shocks 13 → 0).  Iteration 4 re-asks §15.9's question against THAT baseline,
on the datum-invariant instruments §15.10 built.

**The arm.**  `config/test_suite/_golden_sim_campaign_zwdblk_coldfb3.json` = the
tracked `_golden_sim_campaign_zwdblk_scoped.json` + `network.phase_datum_candidacy_usable:
true` (the satellite clause is the code default beneath it) + the run path.  Nothing
else; the flattened key-by-key diff is exactly those two lines.  Exe
`gnss_processor_coldfb3.exe` = `1B37E94B…`, built from main `e90086f`; the reference
was run on `F4FF8EF0…`.  **Those two exes differ only in the posture table** — a
log-only reporter (`logNetworkPosture_` reads already-parsed state and changes
nothing) — and in the cold-fallback code, which is default-OFF and was certified
byte-identical OFF at §15.3.  The reference is therefore a valid same-code baseline,
and it is stated here rather than assumed.

| channel | **S (scoped)** | **B3 (S + candidacy)** | gate | verdict |
|---|---|---|---|---|
| **per-hour-de-biased consumer p50 / rms** | **0.0347 / 0.0466** | **0.0387 / 0.0516** | ≤ S | **FAIL** (+4.0 / +5.0 mm) |
| statically de-biased p50 / rms | 0.0563 / 0.0688 | 0.0815 / 0.0890 | (reported) | worse |
| raw settled p50 / rms | 0.0825 / 0.0899 | 0.1111 / 0.1348 | (reported) | worse |
| settled mean ENU / \|bias\| | +0.0335 / −0.0104 / −0.0460, 0.0578 | +0.0830 / −0.0334 / −0.0473, 0.1012 | (reported) | the east bias returns to the candidacy arms' +0.08 |
| **consumer PB resets** | 1475 | **1337** | < S | **PASS** (−9.4 %) |
| CB discs / clock discs | 517 / 21 | 516 / 21 | (control) | flat |
| shocks (> 0.5 m after h1) | 0 | **0** | | tie, both clean |
| **wire: frac epochs \|step\| > 5 cm** | 0.3611 | **0.2944** | ≤ S | **PASS** (−18 %) |
| **wire: p90 per-epoch max step** | 0.4000 m | **0.2651 m** | ≤ S | **PASS** (−34 %) |
| **PHASE_ANCHOR handovers / cold** | 315 / 78 | 312 / **46** | settled ≤ 19 | **PASS** |
| ...cold-start / settled | 27 / 51 | **27 / 19** | | −63 % settled |
| ...cold by hour h0…h5 | 41 12 7 0 5 13 | **38 8 0 0 0 0** | | zero from hour 2 |
| PB-GAUGE elected / continuous / cold | 462 / 362 / 100 | 468 / 416 / **52** | | |
| continuous successor age p10 / p50 / p90 | 91 / 240 / 459 | **116 / 236 / 478** | | older |
| candidacy: entered / elected / rank-changed / fell through | — | 468 / 399 / **60** / 69 | | |
| satellite clause: sat-ep / families / **splits avoided** / cold-coloc / untracked | — | 133 / 399 / **13** / 0 / 0 | | |
| **split-referenced mean / max** | 0.0 / 0 | **0.0 / 0** | 0.0 / 0 | **PASS** |
| **ZWD settled truth rms / span p50** | 0.0066 / 0.0205 | **0.0060 / 0.0178** | ≤ 0.0066 | **PASS** |
| **ref-free WL / EWL / L1C** | 99.2 / 100 / 90.2 | **99.3 / 100 / 91.0** | ≥ 99.2 / ≥ 90.2 | **PASS** |
| **T4 events / ambient** | 11 / 0.206 | **11 / 0.198** | ≤ 11 | **PASS** |
| **W2 4.9917 h: T4 / decouples / CLK breaks** | 0/0/0 | **0/0/0** | absent | **PASS** |
| **sat-clock datum-free contrast rms** | 0.7657 | 0.7713 (**+0.73 %**) | ±3 % | **PASS** |
| whole-run residual rms | 1.0096 | 1.0144 (+0.48 %) | | |
| **consumer 3D at the GPS pivot edge (319260)** | 0.126 | **0.104** | ≤ 0.126 | **PASS** |
| **consumer 3D at the Galileo pivot edge (321900)** | 0.031 | **0.108** | ≤ 0.031 | **FAIL** (+77 mm) |
| consumer hourly p50 h0…h5 | .524 .113 .114 .101 .089 .047 | .610 .264 .243 .189 .102 .088 | | |
| per-hour-de-biased hourly p50 | .284 .056 .023 .031 .035 .046 | .302 .054 .033 .040 **.033 .040** | | h1/h4/h5 equal-or-better, h0/h2/h3 worse |
| PB resets by hour | 962 0 32 0 256 225 | 953 0 **0 0 232 152** | | better in h2, h4, h5 |
| run time | 1242 s | 1361 s | | +10 % |

**Verdict: 12 of 15 gates pass, 3 fail, and the knob stays KEEP_OFF.**  The three
failures are two instruments: the per-hour-de-biased consumer (p50 +4.0 mm, rms
+5.0 mm — a miss, not a collapse) and the Galileo pivot edge (0.031 → 0.108 m).

**What iteration 4 settles, and it is not nothing:**

1. **On the WIRE the card is now unambiguously better than the best arm this bed
   has.**  It beats S on every wire instrument at once — PB resets 1475 → 1337,
   the fraction of epochs carrying a > 5 cm emitted step 0.361 → 0.294, and the p90
   per-epoch maximum step 0.400 → 0.265 m — while removing 32 settled cold
   handovers and every settled one after hour 1.  That is the third independent
   arm on which the wire instruments move with the cold-handover count.
2. **The producer's truth-graded channels are all better**: ZWD 0.0066 → 0.0060 (the
   best on this bed), ref-free L1C 90.2 → 91.0 (likewise), WL 99.2 → 99.3, T4 equal,
   contrasts +0.73 %.
3. **And the consumer still does not follow.**  Its per-hour-de-biased median is 4 mm
   worse than S's with the wire measurably quieter — the same shape §15.10 found, now
   against a reference that itself fixed the largest consumer defect on the bed.  The
   settled mean east error returns to +0.083 m (the value every candidacy arm carries,
   and the scope-OFF arm too) from S's +0.034, so a third of B3's raw gap to S is
   again a static horizontal realization the card should not be able to control —
   yet the de-biased gap survives, exactly as C5's did.
4. **The Galileo edge is the one real regression**, and it is worth naming precisely:
   at 321900 the scoped response alone reaches 0.031 m and the card takes it to
   0.108 m — still **7× better than the pre-scoped 0.777 m**, but a step back from
   the reference.  The GPS edge moves the other way (0.126 → 0.104).  Both edges are
   epochs where the candidacy pass is re-choosing hosts while a clock datum moves;
   the interaction of the two mechanisms at a pivot edge is untested and is the
   obvious next thing to look at.

**Iteration 5, pre-registered.**  The open question is now narrow and local: *why does
a quieter wire leave a noisier consumer, and what happens at a pivot edge when the
candidacy pass and the scoped handover act in the same epoch?*  Two steps, in order:
(i) a per-edge census of the anchor set across 321900 in S and B3 — which families
moved, whether any moved BECAUSE of the candidacy pass, and whether the consumer's
resets at that epoch are the ones the card added; (ii) only then a knob, if the census
names one: an interlock that defers a candidacy-driven handover by one epoch when the
same epoch carries a clock-datum change, gated on the Galileo edge returning to ≤ 0.031
with the wire instruments kept.

