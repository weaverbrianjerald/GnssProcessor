# §18.12 — the coupling-regressor restore verdict — the mechanism works, the wire is 10× better

**STATUS**

- **id:** §18.12
- **topic:** resume-seam
- **status:** closed
- **summary:** the coupling-regressor restore verdict — the mechanism works, the wire is 10× better
- **dates in the text:** 2026-09-03 → 2026-09-05
- **knobs / posture:** `network.resume_coupling_restore`, `network.readmit_seed_horizon_s`
- **hashes named in the text** (commit / exe / byte-cert): `a8aa408`
- **run dirs named in the text:** `runs/camp60wu_quiet12h_frwriter_sticky`, `runs/camp60wu_quiet24h_sticky`, `runs/cplreg_q12warm_`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` — 18.12

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
### 18.12 COUPLING-REGRESSOR RESTORE — VERDICT (2026-09-03, legs from the v3
`.ep1440` of `runs/camp60wu_quiet12h_frwriter_sticky`, commit a8aa408)

**Result: the card WORKS on the product and is ONE INCREMENT SHORT.**  The
emitted wire improves ~10x on every aggregate instrument; a two-satellite
residual (G20, E31) drives a consumer transient that crosses the pre-registered
h12 stop rule.  `network.resume_coupling_restore` stays OFF pending that
residual, but the mechanism is validated, not falsified.

**The build.**  §18.11 diagnosed the 33 first-fold clock trips as the restored
`LocalCarry` injecting a plain DIAGONAL row: `buildCplRow`'s per-entry loop
never runs on an empty `coupling`, so `rhs = value_abs - x_lin` at the PURE
CONDITIONAL variance — a combination no continuous epoch produces (the legacy
diagonal carry is stiff at the MARGINAL variance; the Card-1 row is stiff at
the conditional variance but SHARED-NEUTRAL, because its `A_s = M_r` cancels the
reduced-normals contribution `alpha (M - M_d)(M - M_d)^T`).  An empty coupling is
the `M = 0` case and drives the border at `alpha ~ 1/cond_var ~ 1e6 m^-2` per row
against the Q3 anchored clock prior's `1/9e-5 ~ 1.1e4 m^-2`.  Snapshot format v3
now serializes the regressors: a `QS` shared-column dictionary keyed on
(identity, s_prev, P_prev), one packed `QL`/`QZ` line per carried row holding
(dictionary id, M_rj) in HARVEST order behind an explicit entry count, and `QR`
for the write-epoch clock-refloat set (a hard-refloated satellite is
skip-stashed, so no other record can carry the flag).

Two design points that are load-bearing and were adjudicated against
alternatives.  (1) `hasCouplingState()` is a SEPARATE predicate from
`hasFrameState()`: the latter also gates the resume-side frame restore
(NetworkRunContext.cpp), so widening it would let a coupling-only snapshot claim
frame state it lacks, the QUEUE-1(d) erase would fire anyway, and the card would
silently defeat itself.  (2) The dictionary key includes `s_prev`/`p_prev`, not
identity alone: a station whose harvest failed keeps an OLDER coupling, so
vintages legitimately coexist — measured 990 dictionary entries against 376
shared columns at the branch epoch, i.e. the multi-vintage case is common, not
theoretical.

**Sizing — the card's own estimate was wrong by ~40x.**  §3 of the handoff
budgeted "~2811 rows x few entries = 250 KB".  The writer run's counters give
526,296,304 present + 84,799 departed terms over 4,020,945 carried-row stashes =
**130.9 terms per row** (run average), and **201.5** at the settled branch epoch.
The `.ep1440` is **15.2 MB**, and a 12 h leg with `state_snapshot_keep_history`
wrote **15 GB** before pruning.  The dictionary is what keeps it there rather
than ~34 MB; do NOT close the gap by sparsifying harder or truncating precision
(that is the Card-1e trip regime).

**Writer gate PASSED.**  `runs/camp60wu_quiet12h_frwriter_sticky/products.txt` is
byte-identical to `runs/camp60wu_quiet24h_sticky/products.txt` over all
34,840,533 bytes of its span, so the capture is provably write-outputs-only.  Its
`.ep1440` carries 2811 `QL` = 2811 `L` rows and 60 `QZ` = 60 `Z` rows (exact 1:1),
990 `QS`, `END` guard exact.

**Three arms, one snapshot, same build** (`runs/cplreg_q12warm_{a,b,c}`), differing
only in the named knobs; graded against the CONTINUOUS run
`runs/camp60wu_quiet24h_sticky`, which is the truth for h12-h24.

| instrument @ seam 302430 | A both OFF | B frame only | C + coupling |
|---|---|---|---|
| clock between-sat SD, G | 1.301 | 0.262 | **0.025** |
| clock between-sat SD, E | 0.551 | 0.144 | **0.030** |
| phase L1C SD, E / G | 9.07 / 8.02 | 0.037 / 0.064 | **0.029 / 0.019** |
| code-side abs(dC) p50 @309630 | — | 0.638 m | **0.062 m** |
| satellites with metre-class dC | — | **12+** (0.74-1.37) | **2** |
| `cta_decouple` @302430 | 0 (nothing restored) | **31** | **2** |
| consumer FINAL 3D | 0.123 | 0.111 | **0.108** |

Arm C removes three of the four clock tails §18.11 named and worsens the fourth:
G14 +1.21 -> small, G17 +1.17 -> small, G22 +1.37 -> small, G01 -0.89 -> +0.10,
but **G20 -1.34 -> -2.25** and **E31 -0.89 -> -1.12**.

**Mechanism telemetry (invariant #4).**  2811 L + 60 Z rows rebuilt from 579,489
terms over a 990-column dictionary, **0 orphans**, **99.3 % of terms bound to a
live column** at the first fold — so this is NOT the rung-3 softening route
(`sigma^2 += sum M^2 P_prev`), which is what §18.11 rung 3 did.  B1b PB sigma
0.008 m at the seam.  2 `tu_reinit_hist` seeds planted (2 LOOSE anchors), 376
border keys folded DIAGONALLY (the known one-epoch C1 asymmetry, now measured).

**The residual, and what it is NOT.**  Trip reasons over the whole run:
arm B = 15 `gap` + 31 `nis`; arm C = 15 `gap` + **2** `nis`.  The 15 `gap` trips
are **byte-identical between the two arms** — same epochs, same satellites
(G20 306240, E31 306450, G12 310890, ... E21 344580) — so they are a pre-existing
bed property this card neither causes nor fixes.  The card's target was the `nis`
population and there it delivered 31 -> 2.

G20 and E31 are simply the FIRST TWO gap trips, ~64 min after the seam, and both
follow an `anchor_handover` away from station S28000SIM one epoch earlier
(306210 and 306420; S28000SIM also handed off G11 at 306120).  In arm B those two
satellites had already been re-seeded by a seam `nis` trip at 302430 and
re-coupled at 302550, so they met the handover on a fresh anchor.  In arm C the
restored anchor is carried intact into the gap re-draw, and that re-draw lands
worse (-2.25 vs -1.34).  **So the residual is a second-order interaction between a
PRESERVED anchor and the pre-existing gap-staleness mechanism at an anchor
handover — not a seam defect, and not a property of the regressors.**  Only the
two gap trips closest in time to the seam produce outliers; the other 13 do not.

**Consumer.**  h12 rms 2.053 m (A 0.769, B 0.672), decaying 1.340 / 0.975 / 0.684
through h15 and 0.309 / 0.162 by h17, with p50/rms 0.96-1.02 at every hour — a
coherent BIAS, not scatter.  Pre-registered stop rule 1 (h12 > 1 m) is CROSSED.
The cause is the two residual satellites: a single-station user cannot absorb one
satellite at -2.25 m, whereas arm B's error is spread over 12 satellites at ~+-1 m
and partially absorbs into the receiver clock.  That is precisely why the wire SD
improves 10x while the single-station consumer degrades, and why arm C
nonetheless ends with the BEST final position (0.108 m).

**Methodological correction, recorded because it nearly shipped the wrong
verdict.**  This card was first graded on the closed-loop consumer alone; h12
crossed the stop rule and the result was written up as a FALSIFICATION with a
root cause ("the restored `s_prev` carries a stale datum realization", from the
census `|delta sum M*s_prev|` p50 4.7 cm).  Measuring the wire reversed it: the
product is ~10x BETTER, so the restored realization is closer to continuous, not
stale-and-wrong; the census number is real but is not the mechanism.  **Grade a
PRODUCER card on the wire first** (`scripts/simnet_seam_tools.py diff` against the
continuous run), the consumer second — the consumer is a derived, single-station
view and is the right FINAL grade but the wrong FIRST instrument.  When two
instruments disagree, the disagreement is the finding.
Two tooling traps that produced the wrong reading: `diff`'s per-constellation row
is `n cm sd max`, so awk field 11 is SD and 12 is MAX (reading MAX as SD inverts
the conclusion — max worsened on one outlier while SD improved 10x); and product
epoch tows are floating point (`302429.99999809`), so an exact `tow_lo == tow_hi`
window returns "common epochs 0".

**Open, in priority order.**
1. G20 / E31: the preserved anchor's first gap re-draw.  Candidate direction is
   refreshing the restored anchor's staleness basis, or re-tapping at the
   `anchor_handover` edge (invariant #7 — a handover is an EVENT and the clock
   anchor's staleness test fires one epoch later with both edges silent).
   **RESOLVED 2026-09-05 (§18.13): the event ledger shows it is the RE-ADMISSION
   edge, not the handover — `col_removed` 305220 → `col_added` 306210 → `gap` trip
   306240 → re-couple on the re-draw; built as E2 (`network.readmit_seed_horizon_s`),
   leg D pre-registered.**
2. `prev_carry_cov` is still not serialized: 376 border keys fold DIAGONALLY at
   the first warm epoch while the restored coupling rows are already conditional.
   Constant across arms, so not an A/B confounder, but it is the other half of
   this seam.  **BUILT 2026-09-05 (§18.13 (b): snapshot QK/QP records under the same
   capture / restore knobs); writer re-run + leg E pre-registered.**
3. Census `rel p90` 1.05e-2 against a pre-registered <1e-3 (p50 5.28e-4).  The
   median row is frame-valid; the tail is not.
4. The 2 residual `nis` trips are on E02/G01 — the FORMER pivots (realized pivots
   at the branch epoch are E03/G02, which were the two LOOSE anchors, since a
   pivot's clock is datum-pinned).  They are NOT the two bad-clock satellites; do
   not assume one defect.

