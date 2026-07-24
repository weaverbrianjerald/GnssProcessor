# REPORT — RTB-7(e) diagonal-only carry blob (`state_snapshot_carry_diagonal`)

Follow-on to `docs/REPORT_warm_resume_event.md` (RS card). All code is
**default-OFF and uncommitted**; nothing shipped.

## 0. Headline

1. **The hypothesis holds at 50 stations: the resumed branch lives in the means
   and variances — not in the correlations.**
   A warm resume from a diagonal-only blob is indistinguishable from one off the
   full dense Pᶻ — CLK slope **29.79 vs 30.18 mm/ep**, ep60 level **2629.5 vs
   2630.3 mm** — against a cold-carry control at **55.81 mm/ep / 3524.7 mm**. The
   cross-covariance is worth **≤3%** of the warm-resume benefit. (Both warm arms
   restored bit-exact *variances*; the A/B isolates the cross-covariances alone —
   see §6 for why the further means-only rung was not climbed.)
2. **The dense Pᶻ it drops is nearly the whole file.** At 50 stations dropping it
   shrinks the carry blob **89.7×** (7,301,090 → 81,430 B) and the whole snapshot
   **56.9×** (7.35 MB → 129 KB) — a **98.2%** byte saving (the dense-Pᶻ *share* of
   the snapshot is 98.6%; §4).
3. **Almost nothing degraded internally — and where it did, the VCE absorbed it.**
   The diagonal arm produced *fewer* D5 innovation flags (1668 vs 1708) and
   *fewer* chord re-births (2373 vs 2403). The one internal statistic that moved
   the *wrong* way is σ̂₀ (+1.6% at 50 sta, +5.4% at 305, read in §3 / §5.2) — the
   anti-conservatism the design flagged, absorbed by the VCE as global inflation
   rather than surfacing in the flag counter (the flag gate is σ̂₀-scaled; §1).
4. **305 confirms it, more strongly.** Where the approximation is most stressed
   (7,869 carried DD, far larger K_c) the diagonal arm is **below** the full blob
   at every epoch from ep13–ep42 on CLK (8–20%) and on CB, with fewer D5 flags
   (7284 vs 7631) and fewer re-births (9323 vs 9662). Snapshot **258.3 MB →
   554 KB = 477.8×**.
5. **Two corrections to the card's premises, both stated in place:** the predicted
   1663× was a numbers-only ratio and the real figure is **477.8×** (§5.1 — the
   `PD` diagonal is only ~31% of the ~517 KB blob; the rest is irreducible
   structure); and at 305 the **slope**
   statistic is not readable inside a 31-epoch window (§5.3 — the cold arm is
   slope-flat but level-high), so 305 is graded on levels.

## 1. What was built

| piece | detail |
|---|---|
| blob **v3** | `serializeCarry(os, diagonal_only=false)` writes ONE `PD,<d0>,…` row per component in place of its `K_c` dense `P` rows. |
| read side | **content-driven**: `restoreCarry` dispatches on the ROW TAG (`P` vs `PD`), never the header version, so v1 / v2 / v3 all restore under either knob setting. |
| knob | `network.state_snapshot_carry_diagonal` (bool, default false). INERT — and force-cleared — without `state_snapshot_carry`. |
| fingerprint | **deliberately excluded.** It is a serialization representation, not a datum-relevant rule; excluding it is what lets one converge run seed both A/B arms. |
| telemetry (inv. #4) | `resumeCarryDiagonal()` / `…DiagonalBlocks()`; the shape is named in the resume line AND the RE-4 run-end line; a write-side line reports snapshot count + last blob bytes + shape; the four RTB-7 carry knobs now appear in the L15 run manifest. |
| edge case | a zero-chord component writes NO `PD` row (the dense path writes no `P` rows either) — otherwise the reader desynchronizes onto the `H` row. |

**Not** built: any inflation of the restored diagonal. See §6.

### The trade, stated honestly

A diagonal restore is **not** a conservative inflation. Dropping a positive
correlation *tightens* the common-mode direction of chords that share tree edges
(for `P = [[1,ρ],[ρ,1]]` the `(1,1)` direction goes `1+ρ → 1`) while loosening the
coordinate directions relative to the true information. It is always PD, so there
is no PSD hazard — but it is anti-conservative along exactly the tightly-coupled
combinations, which is why it was A/B'd rather than assumed.

Where would that risk show up? **Not in the D5 flag counter** — the flag counter
is σ̂₀-coupled by construction. The gate threshold is `(k·σ̂₀)²`
(`src/core/filter/NetworkInvariantCarryDriver.cpp:1172-1173`, applied as
`v_i² ≤ k2f·var` at `:1209` / `:1241`), and σ̂₀ is the M2 LS-VCE inflation
accumulated from the standardized innovations of the *carried* chords themselves
(fed at `:1205-1208` / `:1237-1240`, one pooled `"dd"` scalar for the whole run —
`include/filter/NetworkInvariantCarryDriver.hpp:214-222`). So a *uniform*
over-tightening of the carried prior makes those innovations run hot, raises σ̂₀,
and widens the very threshold the flags are scored against — the tightening is
normalised **out of the flag count and into σ̂₀**. (The coupling is one-sided:
`innovInflationSigma0` floors σ̂₀² at 1.0, `:392-399`, so calibration can only
*raise* the gate, never tighten it below the raw-formal test; every arm here sits
above 1.0, on the live side.) σ̂₀ is therefore where the anti-conservatism *did*
show up — at the few-percent level (§3, §5.2) — and the flag counter guards only
the *localised* case, a handful of chords far enough off to trip `(k·σ̂₀)²` before
the single global scalar can absorb them. The external CLK/CB grading against the
inline reference remains the arbiter, and it favours diag regardless.

## 2. Gates

| gate | outcome |
|---|---|
| **unit** | **PASS** — 888 ran, 887 passed, 0 failed, 1 skipped. Four additive gates: v3 drops *only* the cross-covariance (means/variances bit-exact, cross exactly 0, with a non-vacuity control that the source cross terms were real); default arg byte-identical to the v2 writer + size shrink; **row-tag dispatch across all six {v1,v2,v3} × {P,PD} combinations**; pair (2K, cross-band) and zero-chord blocks. |
| **G0a** structural byte-identity | **PASS** — the pre-change Release binary and the new build, on the *same* config file (new key absent), produce a **byte-identical** `network_solution.csv`; snapshots identical modulo the wall-clock stamp. |
| **G0b** knob OFF vs ON | **PASS** — same build, converge with the knob off vs on: products **byte-identical**. The knob is write-side only. |
| **manifest** | **PASS** — verified by an explicit `write_run_manifest` probe run; the field was otherwise never exercised (the writer is gated off in the A/B configs). |
| **telemetry** | **PASS** — see §3; every arm names its restored shape. |

## 3. The 50-station A/B

Protocol: HB-2 (`scripts/run_warm_resume_arm.sh`, re-seed + verdict assert) with
`state_snapshot_require: true` on every resume arm. Configs
`config/test_suite/_dcb_{A,conv_full,conv_diag,resume,cold}.json`, derived from the
RS-card arms so every datum-relevant knob matches (verified: zero knob diffs).
Converge 0–450 s; resume from +450 s; **46 post-resume epochs** graded.

**The control is exact.** Both warm arms restored **1214 carried DD** with the
identical σ̂₀ seed (**0.8968**) from converge runs whose products are
byte-identical. They differ *only* in whether the 10 blocks' cross-covariances
came along.

CLK between-sat SD-of-SD vs the inline reference [mm]:

| epoch | cold | full | **diag** |
|---:|---:|---:|---:|
| 15 (resume) | 833.9 | 1479.9 | 1494.3 |
| 20 | 1304.7 | 1272.0 | 1234.2 |
| 30 | 1708.0 | 1769.2 | 1807.5 |
| 45 | 2743.4 | 2331.6 | 2322.5 |
| 60 | 3524.7 | 2630.3 | **2629.5** |
| **slope ep20–60** | **55.81** | 30.18 | **29.79** |
| **slope ep30–60** | **53.26** | 21.64 | **20.06** |

CB [mm]: slope ep20–60 cold **28.59**, full **16.59**, diag **17.01**; ep60 cold
1665.8, full 1081.3, diag 1094.2.

> **Epoch labels and slopes.** The epoch index is the grading script's own label
> — `ep = round((tow − the reference run's first-emitted tow) / 30 s)`
> (`scripts/warm_resume_sd_metric.py:91,101`) — and the row tagged *(resume)* is
> the resume arm's **first emitted epoch**. At 50 stations the converge snapshot
> is ep14 (+420 s) and the resume head is ep15 (+450 s), so they are contiguous
> and the restore epoch itself is not re-graded (contrast the 305 case in §5.2,
> where a one-epoch hole opens at the boundary). The **slopes** are
> least-squares fits over *every* epoch in the window, not endpoint differences
> (`fit_slope`, `scripts/warm_resume_sd_metric.py:61-72`); the tables print only
> sampled epochs, so endpoint arithmetic on them (e.g. (2630.3−1272.0)/40 = 33.96
> for the full arm) will *not* reproduce the fitted 30.18 — the table is right,
> the wrong operation is the endpoint subtraction.

Whole-run counters:

| arm | chord births | slip refloats | D5 flags | carried DD @ end | final σ̂₀ |
|---|---:|---:|---:|---:|---:|
| inline | 3321 | 1695 | 1377 | 1206 | 1.76 |
| cold | 3307 | 1711 | 1489 | 1226 | 1.82 |
| full | 2403 | 1972 | 1708 | 1226 | 1.88 |
| **diag** | **2373** | **1942** | **1668** | **1226** | **1.91** |

### Reading

- **Diagonal keeps the whole benefit.** On CLK it is marginally *better* than the
  full blob at both slope windows; on CB marginally worse (+2.5% slope, +1.2% at
  ep60). No consistent direction, magnitude ~1–3% — against a cold-vs-warm
  separation of ~85% in slope. The cross-covariance is not where the branch is.
- **The transient is unchanged** (ep15: 1494 vs 1480, both ~1.8× cold), consistent
  with the RS card's finding that the transient is station-layer re-convergence,
  not a carry property. The RS recommendation stands: discard ~5 post-resume epochs.
- **No hidden cost — the one that moved, moved into σ̂₀.** The predicted failure
  mode was extra D5 flags from over-tightened correlated chords. Flags went *down*
  (1668 vs 1708), as did re-births (2373 vs 2403), and both warm arms end with the
  identical carried DD count. But the anti-conservatism did not simply vanish: the
  final σ̂₀ is **1.91 (diag) vs 1.88 (full)**, +1.6% — the diagonal prior *is*
  hotter along the tightly-coupled directions, and the VCE absorbed exactly that,
  as global inflation, rather than letting it reach the σ̂₀-scaled flag gate (§1).
  The magnitude is ~1.6% and the external grading is unaffected.

## 4. Size

| | full (v2) | diagonal (v3) | ratio |
|---|---:|---:|---:|
| carry blob, 50 sta | 7,301,090 B | 81,430 B | **89.7×** |
| whole snapshot, 50 sta | 7,348,712 B | 129,052 B | **56.9×** |

The dense Pᶻ was **98.6%** of the 50-station snapshot (7,244,543 of 7,348,712 B);
dropping it is the **98.2%** byte saving of headline 2 (marginally less than the
share, because the diagonal still writes one `PD` row). The residual 47,622 B —
snapshot minus the whole carry blob, identical on both arms — is every other layer
(D2 border, emitter, pivots, counters), matching the card's ~48 KB figure.

## 5. 305-station confirmation

Same protocol, configs `_dcb305_*.json` (derived from the RS 305 arms; zero
datum-knob diffs). Converge 0–300 s (snapshot at ep10, +300 s); the resume arm's
configured +330 s fast-forward lands on an exact epoch boundary that the
`< start_offset_s` test resolves to the next epoch, so the realised first live
epoch is **+360 s (ep12)** and **31 post-resume epochs** (ep12–ep42) are graded —
i.e. ep11 (+330 s) is processed by *neither* run, a one-epoch hole between the
converge tail (ep10) and the resume head (ep12); the restore epoch itself is not
re-graded. This is where the approximation is most stressed: K_c is far larger, so
proportionally more correlation is discarded.

**Control again exact.** Both warm arms restored **7,869 carried DD** — precisely the
card's z figure — and both end the run with 7,821. Only the diag arm reports
`DIAGONAL-ONLY (v3, 10 block(s))`. The two converge runs' products are
**byte-identical**, so the knob is write-side-only at scale too.

### 5.1 Size — the card's 1663× was optimistic; measured **477.8×**

| | full (v2) | diagonal (v3) |
|---|---:|---:|
| whole snapshot, 305 sta | 270,878,552 B (258.3 MB) | **566,982 B (554 KB)** |

| row | FULL | DIAG |
|---|---:|---:|
| `P` dense covariance | 270,469,994 B (**99.8%**) | — |
| `PD` diagonal | — | 158,424 B (27.9%) |
| `Z` means | 149,541 B | 149,541 B (26.4%) |
| `A` arc list | 110,358 B | 110,358 B (19.5%) |
| `H` chords | 91,645 B | 91,645 B (16.2%) |
| `X` per-block vertex lists | 6,848 B | 6,848 B (1.2%) |
| carry-blob framing (`B`/`S`/`CARRY`/`CEND`) | 317 B | 317 B (0.1%) |
| every other snapshot layer | 49,849 B | 49,849 B (8.8%) |
| **total** | **270,878,552 B** | **566,982 B** |

Both columns now sum exactly (a byte-per-line-class tally of the two artifacts
closes to `filesize`; the `X`/framing rows were the 7,165 B the earlier table
omitted, identical across arms because none of them depends on the `P`-vs-`PD`
choice). 1663× was a **numbers-only** ratio (ΣK_c² / ΣK_c) that ignored everything
which still has to be written. Inside the ~517 KB blob the `PD` row is ~31% and the
rest is irreducible structure — the `Z` means (~29% of the blob), the `A` arc list,
the `H` chords, plus the small `X` per-block vertex lists — that is the floor, and
it is why 1663× was never reachable. Two figures corroborate independently: the
dense Pᶻ really is **99.8%** of the file, and "every other snapshot layer" lands at
49,849 B against the card's 48 KB.

### 5.2 Accuracy — diagonal is **at least as good as** full

CLK between-sat SD-of-SD vs inline [mm] (resume head = ep12, +360 s; see §5 for
the one-epoch boundary hole at ep11):

| epoch | cold | full | **diag** |
|---:|---:|---:|---:|
| 12 (resume) | 608.3 | 861.6 | 888.9 |
| 16 | 770.2 | 522.9 | **455.8** |
| 25 | 1138.7 | 530.2 | **422.2** |
| 30 | 1397.5 | 986.7 | **845.1** |
| 36 | 1525.3 | 1066.0 | **946.2** |
| 42 | 1521.3 | 1240.5 | **1138.9** |

The diagonal arm is **lower (better) than the full blob at every epoch from ep13 to
ep42**, by 8–20%. CB shows the same ordering, smaller: ep42 cold 722.0, full 445.6,
diag **438.7**.

Whole-run counters — the three carry-health tallies (births, refloats, flags) all
move the same way; the **fourth** moves the other way:

| arm | chord births | slip refloats | D5 flags | carried DD @ end | final σ̂₀ |
|---|---:|---:|---:|---:|---:|
| inline | 16298 | 7271 | 6328 | 7821 | 1.35 |
| cold | 13788 | 5123 | 4420 | 7821 | 1.10 |
| full | 9662 | 8320 | 7631 | 7821 | 1.48 |
| **diag** | **9323** | **7981** | **7284** | **7821** | **1.56** |

The fourth tally is **σ̂₀: 1.56 (diag) vs 1.48 (full), +5.4%** — larger than the
+1.6% at 50 stations, and scale-ordered with the discarded-correlation fraction
(1,214 carried DD at 50 → 7,869 at 305). That is the §1 anti-conservatism, and the
three co-moving tallies all sit *behind* the σ̂₀-scaled gate, so a uniform
tightening lands in σ̂₀, not in them. σ̂₀ measures internal *consistency*, not
accuracy: the **cold** arm has the *lowest* σ̂₀ (1.10) while sitting at the
*highest* error level vs inline — fresh diffuse priors simply match fresh
errors. The external CLK/CB grading remains the arbiter and favours diag.

### 5.3 Slopes are NOT readable in this window — stated, not hidden

| window | cold | full | diag |
|---|---:|---:|---:|
| CLK ep20–45 | 24.79 | 38.29 | 36.90 |
| CLK ep28–42 | **8.73** | 28.45 | 30.87 |
| CLK ep30–42 | **10.60** | 22.70 | 26.31 |
| CB ep28–42 | 12.07 | 3.74 | 4.07 |

At 305 the **cold** arm has the *shallowest* slope in every **CLK** window while
(past the first few epochs) sitting at the *highest* level — it is already
saturated near its plateau, whereas both warm arms are climbing from a far better
starting point toward the same asymptote. (The lone CB window inverts even that —
cold 12.07 is the *steepest* — which only sharpens the point: the late-window
slopes carry no stable signal here.) That
inverts the 50-station reading, and it means the slope statistic has **not** reached
the asymptotic regime within 31 epochs: a bigger network re-converges more slowly,
so 31 epochs at 305 is not the equivalent of 46 epochs at 50. **Level is the
meaningful discriminator here; the late-window slopes are not.** Read that way the
answer is unambiguous — diag is below full on both kinds at essentially every epoch.

### 5.4 Why might dropping the covariance *help*?

A hypothesis, **offered and not proven** by this single realization: the converge
run's dense Pᶻ encodes a correlation geometry conditioned on a per-station filter
layer that the resume deliberately does **not** restore. Carrying that geometry
across the resume asserts structure the resumed data no longer supports, which
over-constrains the fold; the diagonal restore is simply more honest about what
actually survived. That is consistent with the RS card's finding that the transient
is station-layer re-convergence tension rather than a carry property, and with the
diag arm's *lower* D5 flag and re-birth counts. Direction is corroborated by three
independent counters; magnitude is single-realization and should not be quoted.

A second, **code-grounded** mechanism sits alongside it (also offered, not
proven): the VCE re-scales the carried-covariance confidence every epoch — the D5
gate and the re-tap seeding are both ∝ σ̂₀² (`(k·σ̂₀)²` at
`src/core/filter/NetworkInvariantCarryDriver.cpp:1172-1173`; re-tap
`Pᶻ(i,i) = σ̂₀²·var` at `:1213` / `:1245`). So a *scalar* miscalibration
introduced by dropping the correlations self-corrects within a few epochs; what
cannot self-correct is direction-specific geometry — which the staleness
hypothesis above argues was stale anyway. The two mechanisms are not rivals: one
says the dropped geometry was wrong to carry, the other says a merely-mis-scaled
prior would have been repaired regardless. The early window still favours the
staleness story — the diag advantage is already present by ep13, before the arms'
σ̂₀ have diverged much (they end 1.56 vs 1.48). Both remain single-realization
readings.

## 6. Deferrals (invariant #3)

| item | class | condition |
|---|---|---|
| inflating the restored diagonal (e.g. ×κ, or pairing with the RE-2 shock) | **struck** — field-inert | the plain diagonal already matches (50 sta) or beats (305) the full blob. There is no deficit to inflate away, and RE-2 was already ruled STOP by the RS card. |
| a low-rank / banded Pᶻ shape (keep the strongest correlations) | **struck** | it would recover exactly the thing measured to be worth ≤3% (50) or negative (305), at the cost of the format's simplicity. |
| a means-only (v4) arm — restore `z`, seed the diagonal by the existing re-tap rule σ̂₀²·var (`NetworkInvariantCarryDriver.cpp:1213`) | **struck** | the headline says "means and variances"; both warm arms here restored bit-exact *variances*, so the A/B isolated the cross-covariances only (≤3% at 50, ≤0 at 305) and the means-vs-variances rung was never climbed. Not worth climbing: `PD` is 158,424 B = 28% of the 566,982 B snapshot, so the size motivation is exhausted, and the full-vs-diagonal decision is unaffected. It becomes a *mandatory* prerequisite only if a future card wants to **modify** restored variances (e.g. inflate them) — the same family the inflation row above strikes. |
| a LONGER 305 window (≥60 post-resume epochs) to read asymptotic slopes | follow-up, **field-inert for this card** | §5.3 — 31 epochs cannot resolve slopes at 305. It does not gate the full-vs-diagonal decision (levels are unambiguous and counters corroborate), but any future *warm-vs-cold* magnitude claim at 305 needs it. ~85 min of Release time. |
| shrinking the remaining blob structure (`Z` means, `A` arc list, `H` chords, `X` vertex lists) | not pursued | 477.8× is already past the point where snapshot size constrains anything; `A`/`H`/`X` are structural (they define A_old membership, the chord basis, and the per-block vertex numbering) and would need a real encoding change, not a drop. |
| frame-invariant branch metric | **inherited, still blocking** any across-run branch claim | unchanged from the RS card §8 — `diff_carry_blobs.py` remains within-run only. |
| `diff_carry_blobs.py` v3 support | **no change needed — MEASURED** | run unmodified on the v2 and v3 blobs of the same converge state: 1214/1214 common chords, **max \|Δz\| = 0.0000 cyc, 100% same-integer**. Its parser matches `"P,"`, which `"PD,"` does not, so PD rows are skipped and it never read the covariance anyway. This also confirms at production scale that v3 preserves the means bit-for-bit — the round-trip the unit gate asserts on synthetic blocks. |

## 7. Artifacts

Code (default-OFF, uncommitted): `NetworkInvariantCarryDriver::serializeCarry(os,
diagonal_only)` + content-driven `restoreCarry` + `resumeCarryDiagonal{,Blocks}`;
`network.state_snapshot_carry_diagonal`; write-side snapshot telemetry in
`NetworkRunContext` + the RE-4 run-end line in `NetworkTeardown`; four RTB-7 carry
knobs in the L15 manifest (`NetworkRunTraces`).
Configs: `config/test_suite/_dcb_*.json` (50 sta), `_dcb305_*.json` (305 sta).

## 8. Long-run warm-start demonstration (overnight, 2026-07-24)

The A/B above used short (300–450 s) converges. This exercises the whole cycle at
the operating duration the feature exists for: a **6 h converge (720 epochs)** →
snapshot → **6 h warm resume**, diagonal blob throughout. Configs
`_night50_*.json` (done) and `_night305_*.json` (305 running). Release, renamed exe,
`state_snapshot_require: true`, HB-2 immutable re-seed. This is a *demonstration*
(no inline reference arm), so it grades correctness/continuity, not accuracy.

### 8.1 50-station, 6 h → 6 h (complete)

| phase | wall | result |
|---|---|---|
| converge | 64.5 min (720 ep) | 720/720 emitted, **0 withheld / 0 skipped**; 1255 carried DD at end; σ̂₀ 2.27; 720 diagonal snapshots, last **83,725 B** |
| warm resume | 63 min (720 ep) | **WARM + BRANCH-FAITHFUL + DIAGONAL**, verdict-asserted PASS |

**Did it converge?** Yes — the network emitted every one of 720 epochs with nothing
withheld by the observability gate, carried DD held steady at ~1255 (births ≈
refloats — the designed refloat-not-carry-stale treadmill, not growth or collapse),
and in steady state consecutive epochs added **zero** new product discontinuities
(disc sum 2876 → 2876). Slip activity was high on this data (~165 chord refloats/ep)
but fully absorbed by the refloat path (invariant #1) — 0 epochs lost.

**Did the warm-start pick up correctly?** Yes, with one honest nuance:
- carry restored **exactly** (1255 DD = converge end), σ̂₀ **seeded** 2.27, D2
  border (486), pivots, and emitter state all warm; **generation id preserved**
  (no consumer hard-reset); restore was **DIAGONAL-ONLY (10 blocks)**;
- snapshot **age 90 s** vs the 900 s horizon — a true continuation, not a stale
  reload;
- product count **continuous across the seam** (433 rows/epoch on both sides);
- *nuance:* the first post-resume epoch flags a discontinuity on ~93% of products
  (disc +401), settling to +2 within two epochs. That is the emitter correctly
  telling consumers about the 90 s emission gap — the expected ~3-5 epoch resume
  transient the RS card says to discard, **not** a cold-start (a cold-start would
  also have reset the datum and lost the 1255-DD branch, neither of which happened).

### 8.2 305-station, 6 h → 6 h (scale demo)

Converge complete (6.5 h wall, 723 epochs aligned, ~20 GB working set / 127 GB
machine — no swap); warm resume running (correctness confirmed, full 6 h lands
~14:00).

**The operational payoff, at scale.** The 6 h converge wrote **720 snapshots at
0.56 MB** (carry blob 0.51 MB, `CARRY,v3`, 10 diagonal blocks). The dense (v2) blob
at this scale was measured at **258.3 MB** (§5.1) — so a dense-blob run would have
rewritten ~258 MB *every emitted epoch*, ~186 GB of atomic temp+rename churn over
the run. The diagonal blob makes a snapshotting 305-station producer practical;
the dense one does not. This is the reason the feature exists, shown end-to-end.

**Clean converge.** 720/720 epochs emitted, **0 withheld / 0 skipped**; 8318 carried
DD at end; σ̂₀ 2.29. Same clean picture as 50 sta, at 6× the stations.

**Warm-start correctness (confirmed from the opening epochs).**
- **BRANCH-FAITHFUL: 8318 carried DD restored = converge end**, from the 0.56 MB
  diagonal snapshot;
- σ̂₀ **seeded** live 2.2883; **DIAGONAL-ONLY** restore (10 blocks); **generation id
  preserved**; 500 D2 shared-carry + pivots + emitter state warm; C1 dense border
  re-seeds diagonally at the first emitted epoch;
- snapshot **age 90 s** vs the 900 s horizon — a continuation, not a stale reload.

**Open question — does warm-diagonal *help* vs cold at 305/6 h?** This
demonstration confirms the warm-start *mechanism* at scale but does **not** grade
accuracy: there is no cold-carry or inline arm here (the request was a warm-start
continuation, not an A/B). That grading matters because a separate GPS+Galileo 305
warm-resume test (`project_gg305_warm_resume_chain`) found warm-diagonal *worse*
than cold in a 1 h window — inverting the GPS-only §5 result — with the mechanism
working identically (14,241 DD restored, σ̂₀ seeded). So whether the §5 "diag ≥
dense ≥ cold" ordering survives a 6 h GPS-only window is genuinely open; resolving
it needs a cold-carry control (and ideally an inline reference) over this same 6 h
window — two more ~6.5 h Release runs, offered but not auto-run.
