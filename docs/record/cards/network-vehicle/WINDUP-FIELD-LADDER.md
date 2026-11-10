# wind-up field: the three-arm field ladder and its replications

**STATUS**

- **id:** wind-up field
- **topic:** network-vehicle
- **status:** closed
- **summary:** NULL REJECTED: the truth-free ladder's slope lands near 2 (the inverted-sign model), never 0, in 9 of 9 components at three stations; the replication and the network-producer arm are consistent with it.
- **dates in the text:** 2026-08-21 → 2026-08-22
- **knobs / posture:** `estimation.accumulate_phase_windup` (loader default false: every field lineage ran fractional when this was written), `network.trace_station_residuals` (zero rows on the `carry_invariant` chain); arms = wind-up ON shipped, OFF, ON fixed; `eigen_threads: 1`
- **hashes named in the text** (commit / exe / byte-cert): `919bb885` (pre-fix byte-cert exe, now an expected-fail baseline), `90e68d3` (the fractional-mode loader warn); ladder exes `D50F4C56…` (shipped) and `4380A2B6…` (fixed); matrix and network-arm exes `727A9E96…` (shipped, built at `3861718`) and `AA472278…` (fixed); fixed simulator `21C2DCD0…`
- **run dirs named in the text:** `runs/simnet60_campaign_wu93`, `runs/simnet60_quiet_wu93` (regenerated fixtures), `runs/camp60wu_smoke_mixed`; `test_suite_results/windup_ab/`; configs `config/test_suite/manifest_windup.json`, `_wuladder_{corr,off,ship}.json`
- **source:** `docs/archive/SPEC_phase_windup_sign.md` §9 to §9.10

**Outcome.** The question: does real data separate the shipped (inverted) wind-up sign from the corrected one, by a truth-free ladder? The arc-demeaned phase residual RMS orders fixed < OFF < shipped in 7 of 7 configurations. A five-station replication puts the fixed arm ahead of OFF in horizontal RMS at every station (mean 2.26 → 1.30 cm); the network-producer arm is CONSISTENT, not decisive. Absolute wire-quality re-grades are not certified. The fractional-wrap follow-on (section 9.5) went to [wind-up census](../slip/WINDUP-FRACTIONAL-CENSUS.md); accumulated wind-up was ARMED on the goldens by [WUP](../../refound/WUP_windup_accumulated_promotion_2026-10-01.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_phase_windup_sign.md -->
## §9 THE FIELD RESULT — the three-arm ladder, 2026-08-21

Ran per §8.2's protocol.  Manifest `config/test_suite/manifest_windup.json`,
analyzer `scripts/windup_ab_linearity.py`, 28 runs, three known-coordinate stations
on 2024-129 graded against ITRF2020 SINEX.  Exes:
`gnss_processor_shipped.exe` `D50F4C56…`, `gnss_processor_windupfix.exe` `4380A2B6…`.

```
arm1 = wind-up ON,  shipped exe (inverted sign)
arm2 = wind-up OFF
arm3 = wind-up ON,  sign-corrected exe
```

### §9.1 Why the ladder, and not "ON must be worse than OFF"

**The original §8 item 1 criterion is not guaranteed and was replaced.**  With `u`
the ENU shift from one unit of wind-up and `v` all other error vs truth,

```
|v + 2u|² − |v + u|² = 3|u|² + 2 v·û
```

so ON is *better* than OFF whenever `v·û < −1.5|u|`.  It holds on a well-controlled
run only because `|u|` happens to exceed the competing error — not by construction.
**Measured: it fails at JPLM, in both postures** (§9.3).

The ladder is truth-free and sign-free instead.  The arms differ by an additive
per-observation term, so for a linear estimator

```
(I)  second difference  arm1 − 2·arm2 + arm3 ≡ 0
(II) ladder slope       (arm1 − arm3) = 2 · (arm2 − arm3)
```

and under the NULL (shipped sign already right) `arm3 ≡ arm1`, giving slope **0**.
**Slope 2 vs slope 0, with nothing in between.**

### §9.2 Controls — both pass

| control | result |
|---|---|
| wind-up OFF, shipped vs fixed exe, `state_trace.csv` | **BYTE-IDENTICAL, 6/6 cells** (3 stations × frac/acc) |
| wind-up ON, shipped vs fixed exe | **DIFFERS, 3/3** |
| `b_wu_off_frac` vs `d_wu_off_acc` within one exe | identical — `accumulate_phase_windup` is a no-op when the call is gated off |

The first two together prove the binaries differ in nothing but the wind-up path,
and that the runs are deterministic (`eigen_threads: 1` pinned in every cell).

### §9.3 Result — slope lands on 2, never on 0

Accumulated posture (the verdict posture — see §9.5):

| station | slope E | slope N | slope U | R² E/N/U |
|---|---|---|---|---|
| **GCGO** | 2.086 | 2.024 | 2.068 | 0.937 / 0.998 / 0.961 |
| **JPLM** | 2.071 | 1.951 | 1.559 | 0.993 / 0.949 / 0.904 |
| **CIT1** | 1.886 | 1.661 | 1.669 | 0.392 / 0.646 / 0.050 |

Nine of nine components land near 2; none is near 0.  **The null is rejected at all
three stations.**  The shipped model applies the negative of the corrected one, on
real data, inside the solve.

**Arc-demeaned post-fit CARRIER-PHASE residual RMS — the sharpest observable, and
it is unanimous.  7 of 7 configurations, monotone in the predicted direction:**

| configuration | arm3 fixed | arm2 OFF | arm1 shipped | rows/arm |
|---|---|---|---|---|
| JPLM accumulated | **13.042** | 13.521 | 14.609 | ~110 800 |
| JPLM fractional | **13.010** | 13.521 | 14.437 | ~110 800 |
| CIT1 accumulated | **13.122** | 13.936 | 14.851 | ~63 850 |
| CIT1 fractional | **13.175** | 13.936 | 14.945 | ~63 850 |
| GCGO accumulated | **9.771** | 10.816 | 11.993 | ~116 650 |
| GCGO fractional | **10.078** | 10.816 | 12.055 | ~116 650 |
| JPLM kinematic acc | **11.068** | 11.256 | 12.378 | ~110 815 |

mm, arc-demeaned, from `logs/measurement_residuals.csv`.  This is the observation
domain, where the effect is a 5–15 σ signal against `phase_std_m = 0.002`, rather
than the position domain where it is millimetres against a centimetre bias.

**Truth-graded ENU (the price, not the proof)** — `|ENU error|` in mm:

| configuration | arm3 fixed | arm2 OFF | arm1 shipped | ordering |
|---|---|---|---|---|
| CIT1 acc | **46.79** | 53.70 | 59.97 | monotone ✓ |
| CIT1 frac | **46.69** | 53.70 | 58.33 | monotone ✓ |
| GCGO acc | **56.67** | 60.02 | 66.07 | monotone ✓ |
| GCGO frac | **57.25** | 60.02 | 66.27 | monotone ✓ |
| JPLM acc | 49.64 | 45.84 | **45.33** | INVERTED |
| JPLM frac | 49.47 | 45.84 | **46.11** | INVERTED |
| JPLM kin | 73.85 | 60.08 | **57.40** | INVERTED |

**The inversions are predicted a priori and are not evidence against the fix.**
Evaluating `v·û < −1.5|u|` with each configuration's own measured `u` and `v`
predicts the ordering correctly in **7 of 7** cases — monotone where it says
monotone, inverted where it says inverted:

| configuration | \|u\| mm | v·û | threshold −1.5\|u\| | predicted | observed |
|---|---|---|---|---|---|
| JPLM acc | 9.68 | **−23.60** | −14.51 | INVERT | INVERT ✓ |
| JPLM frac | 9.69 | −22.67 | −14.54 | INVERT | INVERT ✓ |
| JPLM kin | 14.41 | −71.18 | −21.61 | INVERT | INVERT ✓ |
| CIT1 acc | 8.98 | +34.15 | −13.48 | monotone | monotone ✓ |
| CIT1 frac | 9.05 | +34.39 | −13.58 | monotone | monotone ✓ |
| GCGO acc | 10.26 | +13.90 | −15.38 | monotone | monotone ✓ |
| GCGO frac | 14.45 | +4.02 | −21.67 | monotone | monotone ✓ |

Both inversions are at JPLM, and both are driven by JPLM's **pre-existing,
unexplained +45 mm (static) to +73 mm (kinematic) standing Up bias** — a defect of
the vertical, not of the wind-up sign, and already on the books
(`project_vertical_instability_solidtide`).  The wind-up shift at JPLM is
Up-weighted (`û_U = −0.37`), so a corrected model pushes the solution *along* that
pre-existing bias.  This is precisely why the truth ordering cannot carry the
verdict, and why the ladder and the residual RMS do.

**The price, in the corrected frame:** `|arm2 − arm3|` = **8.98 – 14.45 mm** of ENU
per unit of wind-up, so the shipped code was carrying roughly **2×** that — ~18–29
mm of position error at these three sites, and 8–20 % of the arc-demeaned phase
residual RMS.

### §9.4 ⚠ Why the second difference is NOT at the numerical floor — arm-dependent editing

Identity (I) predicted micrometres; measured max `|arm1 − 2·arm2 + arm3|` ran from
3.8 mm (GCGO N) to 1195 mm (CIT1 U).  **Diagnosed, not hand-waved: the arms do not
process the same observations.**  Row counts in `measurement_residuals.csv`:

| station | arm1 shipped | arm2 OFF | arm3 fixed |
|---|---|---|---|
| JPLM | 222 207 | 222 226 | **222 228** |
| CIT1 | 127 610 | 127 616 | **127 620** |
| GCGO | 233 325 | 233 338 | **233 339** |

Monotone: the arm carrying the largest residual loses the most observations.  QC is
arm-coupled — a 2× residual trips elevation / variance / innovation gates that a 1×
residual does not — so strict superposition cannot hold and identity (I) is an
approximation, not an identity, on a QC-armed run.  CIT1 is worst because it is
GPS-only RINEX 2 at 15 s with the poorest convergence (17 160 s).

Two consequences worth carrying:

- **Identity (I) is only exact with QC disarmed.**  A future ladder that wants the
  micrometre check must run with snooping / AR / slip repair off; the identity
  closing there and not here would itself be the reportable finding.
- **It makes arm1 look BETTER than it is** — the shipped arm is also having its
  worst observations discarded.  Every arm1 number above is therefore conservative.

### §9.5 ⚠ The sign fix and the §7 WRAP are coupled in the deliverable, though independent in mechanism

Residuals per posture (`W` = wrapped wind-up, `N` = the integer staircase, `φ = W + N`):

| posture | arm1 ON shipped | arm2 OFF | arm3 ON fixed |
|---|---|---|---|
| **accumulated** | `2φ` | `φ` | **0** (per-arc constant, ambiguity-absorbed) |
| **fractional** | `N + 2W` | `φ` | **`N`** — the one-cycle staircase |

In ACCUMULATED mode the corrected arm's residual is fully absorbed.  In FRACTIONAL
mode it is `N`: mostly a per-arc constant, but stepping a **full cycle** (0.19 m at
L1) at each half-cycle crossing — 43 measured jumps/day on one station's trace,
~753/day scaled to 63 stations, mid-pass and in good data (§7).

`estimation.accumulate_phase_windup` defaults **FALSE**
(`GnssConfigLoad.cpp:1135-1136`), so **every field lineage runs fractional.**  The
measured phase residuals above still favour the corrected arm in both postures
(13.010 vs 13.521 vs 14.437 at JPLM fractional), so the fix is a win as landed —
but the fractional corrected arm is trading a smooth systematic for a
discontinuity, which is a standing-invariant #1 shape.

**⇒ Follow-on card, and it should be next: revisit the
`accumulate_phase_windup` loader default.**  The two defects are independent —
arming accumulation does not fix the sign, and the sign fix does not remove the
wrap — but the sign fix is what makes the wrap the dominant remaining wind-up
error on a field lineage.  Do not close §7 by assuming this card covered it.

### §9.6 What this run does and does NOT certify

- ✅ The PPP / PPP-RTK **user** path, three stations, static and kinematic, both
  accumulation postures.
- ❌ The **network producer**, which §4 argues is where this actually lands (the
  emitted satellite clocks and phase biases).  `enableWindUpTracing` has exactly one
  caller — `Application.cpp:1630-1631`, inside `Application::run()` *after* the mode
  branches — so the network, baseline and real-time paths never write
  `windup_trace.csv`, and `ApplicationNetwork.cpp` never calls `logResiduals`, so
  there is no `measurement_residuals.csv` either.  The network analogue is
  `network_station_residuals.csv` (`NetworkRunTraces.cpp:696`, gated on
  `network.trace_station_residuals`); a σ̂₀ / `s_norm` ladder across the three arms
  is the owed network arm.
- ❌ **Absolute wire-quality re-grades** (§8.5).
- ⚠ **GCGO's SINEX-vs-SIF cross-check is 5.45 cm** — flagged by the harness's own
  5 cm tolerance, because its last SSC solution window ends 20:366 and 2024 is an
  extrapolation.  Its *differential* ladder is unaffected (it needs no truth); its
  absolute bias is truth-ambiguous at a scale larger than the entire effect.  Quote
  absolute grades from JPLM (0.50 cm SINEX/SIF agreement) and CIT1 (1.38 cm).

### §9.7 Replication + extension — the 5-station 4-arm matrix (2026-08-21, second session)

Independent re-execution with a different harness (`scripts/windup_ab_slice.py`,
`test_suite_results/windup_ab/`), different exes (shipped `727A9E96…` built at
`3861718` — the last pre-fix commit — in a worktree; fixed `AA472278…` from the
fixed working tree), five stations
(JPLM, ALGO, ALIC, ABPO, ZIMM — three not in §9's set), daily static PPP
GPS+Galileo on 2024-129 vs ITRF2020 SINEX, warm-up 1800 s.  Four arms close the
loop the ladder's three cannot: `shipped_off` vs `fixed_off` is a SAME-SOURCE
identity gate across the two binaries.

- **Identity gate: EXACT.**  `shipped_off ≡ fixed_off` in every metric at every
  station (3D-RMS 4.83/11.67/3.70/5.67/2.37 cm) — the sign is the only mover
  between the two builds on the user path.
- **Horizontal RMS (the channel wind-up actually loads; cm):**

  | arm | JPLM | ALGO | ALIC | ABPO | ZIMM | mean |
  |---|---|---|---|---|---|---|
  | shipped_on  | 2.1 | 1.9 | 4.4 | 6.0 | 1.5 | 3.18 |
  | off (both)  | 1.1 | 1.2 | 3.1 | 4.3 | 1.6 | 2.26 |
  | fixed_on    | 1.0 | 0.7 | 0.8 | 2.7 | 1.3 | 1.30 |

  `fixed_on` beats OFF at **5/5** stations; `shipped_on` is worse than OFF at
  4/5 (ZIMM ~tie) — the §8 item 1 shape, now at five geometries.  The East bias
  collapses under the fix (ALIC −4.1 → −0.7 cm, ABPO −5.7 → −2.1 cm).
- **§9.1's caveat reproduces**: in 3D at JPLM, `fixed_on` (5.16 cm) > OFF
  (4.83 cm) — the `v·û` geometry, not a counter-signal; JPLM's H-RMS still
  improves (1.1 → 1.0 cm).  3D is diluted by arm-invariant vertical biases
  (ALGO carries +11.5 cm U in all four arms).

### §9.8 §8 item 2 CLOSED — the contract now covers every consumer path

The `kWindUpYawModelId = "nominal-wu93"` token (S-record field 3) is pinned by
the BINARY, not adopted from the file, on all three consumer entry points:

- `ApplicationRealtime.cpp` `checkConsumerContract` — pin at the expectation
  seed; the check now runs on EVERY consuming posture (previously gated on the
  `network_consumer` profile, leaving the legacy default-flags tail silent),
  and an S-less (v1) wire draws a one-shot UNVERIFIABLE warn instead of
  default-matching silently.
- `ApplicationConsumerReplay.cpp` — previously read the contract and never
  called `checkContract` at all: the offline replay driver, the path most
  exposed to archived pre-fix products, consumed them with zero trip.  Now
  adopts the file contract pivot-less, pins the wind-up token, hard-warns
  (never aborts) on mismatch, and warns on a v1 wire.
- `networkSnapshotFingerprint` (`NetworkRunContext.cpp`) — the token is in the
  warm-resume fingerprint, so resuming a pre-fix snapshot cold-starts (or, on
  require-armed lineages, hard-errors LOUDLY — intended).

Ops consequences, both intended and both by design LOUD:
- Every stored pre-fix product / golden byte-cert hash (e.g. the
  `_camp60_bytecert*` captures, exe `919bb885`) is now an **expected-fail**
  baseline — the fix is solve-affecting on every path.  Re-capture, do not
  re-grade.
- Live snapshots on disk written pre-fix will not warm-resume across the fix
  (8 present on 2026-08-21).

### §9.9 The sim is frame-consistent again — fixtures regenerated

`runs/simnet60_campaign_wu93` + `runs/simnet60_quiet_wu93` regenerated with the
fixed simulator (`21C2DCD0…`).  A pre-fix fixture consumed by a fixed producer
carries −φ injected vs +φ modelled = a 2φ_wu band-common phase error in every
observation — the same mismatch class the product token guards, one layer down.
Any sim arm that mixes a pre-fix fixture with a post-fix producer (or vice
versa) is confounded; the fixture and the producer must sit on the same side of
the fix.  (Same-side pairs cancel identically — §2 stands: the sim remains
structurally blind to the sign itself.)

**Measured (`runs/camp60wu_smoke_mixed`, 10 ep, pre-fix fixture x post-fix
producer): the mixed pairing is INVISIBLE to every short-horizon internal
instrument** — code σ̂₀ 0.9555 vs 0.9558 matched-clean, phase σ̂₀ 0.0080
identical, epoch-1 clock-Δ identical (wind-up never touches code, and over
5 min the 2φ_wu error is ~constant per (station, sat), so the free ambiguities
absorb it whole).  It expresses only in the emitted wire frame (clocks / phase
biases) and over arc-length leveling horizons — §4's split, reproduced.  So the
fixture-side discipline cannot be enforced by watching σ̂₀; it is enforced by
provenance (the `_wu93` fixture suffix + this section), which is the same
invariant-#4 lesson the product token encodes one layer up.

### §9.10 The NETWORK-producer arm (§9.6 item 1) — EXECUTED 2026-08-22

Three 3 h golden63 GPS laneoff legs (`_wuladder_{corr,off,ship}.json`, 362/362
emitted each, cold start each — per-arm `state_snapshot_path`, since the
shipped exe lacks the fingerprint token and a shared snapshot would have let it
warm-resume asymmetrically).  Exes: corr/off `AA472278…` (windup on/off), ship
`727A9E96…` (pre-fix, windup on).

- **The one phase-sensitive data-side instrument orders exactly as the slope-2
  model predicts**: block-Baarda `geo(t_B)` = **2.187 (corr) < 2.215 (off) <
  2.271 (ship)** — corrected best, shipped worst, ~4 % spread at 3 h.
- **Null controls behave**: the prires 'cb'/'rxcb' families (CODE-prior
  residuals — wind-up-free channels) sit flat to slightly reversed (±0.2–2 %),
  and D5's carry σ̂₀ moves < 1 %.  The signal is confined to the channel the
  physics says it should load.
- **Single-leg WIRE comparisons are gauge-noise dominated**: PB med|Δ| after
  epoch common-mode removal is 25.4 cm for the 2φ pair (corr–ship) vs 22.4
  (corr–off) and 19.1 (ship–off) — ordered as a wind-up excess on top of a
  ~19 cm per-leg gauge-realization floor, but nowhere near the clean 2:1:1 of
  a pure signal.  A wire-frame grade needs matched-gauge processing (same
  datum realization), not independent legs.
- Posture note: `network.trace_station_residuals` opens but writes ZERO rows
  on this carry_invariant chain — the writer rides the LEVELING RAIM pool,
  which is structurally empty here (the known pool re-host gap).  The σ̂₀
  ladder above is the working instrument set.
- The §7 FRACTIONAL-mode loader warn (commit `90e68d3`) fired at runtime on
  this chain — the field lineage inherits wrapping mode exactly as §7 claims.

Verdict: the network arm is CONSISTENT with the inverted-sign model and the
fix, with the effect small at 3 h on carry-tension statistics — the USER-path
ladder (§9, slope 9/9) remains the decisive instrument.  §9.6's "owed network
arm" is closed as EXECUTED-and-measured, not as a second decisive proof.

---

