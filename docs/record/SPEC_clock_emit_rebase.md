# SPEC — Card CLK-R: clock emission re-base + per-sat step marks
### (the internal clock re-realization card — mediate the common-mode class at emission; give the per-sat class wire marks)

2026-08-09.  Successor to the F15 verdict (`CONFIG_promotion_ledger.md` §A2 F15)
and edge-queue Edge 2.  The internal (golden63 self-contained) lineage's emitted
clock re-realizes on **613/1440 census epochs** (common-mode steps > 0.1 m; 273
epochs with per-sat outliers > 0.5 m) riding on **one** wire mark — the clock
channel has no magnitude jump test by design (RTA-11), so the whole class is
wire-invisible.
Design input: FC-1's churn ceiling (`docs/SPEC_fast_clock_split_cadence.md` §13,
absorbed there 2026-08-15 from `REPORT_fc1_split_cadence.md`, now in
`docs/attic/`; memory `project_fast_clock_card`) — ~98 % of the emitted clock's
epoch-to-epoch motion is producer frame churn (physical ~1.7–2 cm / 30 s,
corr ≈ 0), i.e. nearly all wire motion is not information.
⚠ Those ~2 cm are a MEASUREMENT FLOOR, not the walk: the accumulating physics is
q_phys ≈ 2.9e-8 m²/s = 0.94 mm / 30 s (`docs/SPEC_clock_temporal_anchor.md` §7).
§1/§2's measured numbers are unchanged; only what "physical" means in them.

## §1 Measured basis (all numbers from `runs/golden63_12h_internal/products.txt`)

Re-derived 2026-08-09 with `scripts/product_step_census.py`-aligned statistics
(the census's own 613 reproduced exactly).

- **Common-mode channel** (per-epoch median of per-sat Δc0): |step| med 0.085 m,
  p90 0.176, **max 17.70 m**; > 0.1 m on 613/1439 steps.  Accumulated offset is a
  bounded wander: **+22.8 m over 12 h** (max |·| 23.2).
- **Per-sat channel** (Δc0 − median): a random walk, robust step σ = **0.092 m /
  30 s** (1.4826·MAD over 45 271 rows) — the same object C1 measured at 0.157 m
  on DOY-129 — vs FC-1's ~2 cm physical.  Tail: dev > 0.5 m on 882 rows /
  267 epochs (73.5/h), > 1.5 m on 84 rows (7.0/h), > 5 m on 8 rows, max 10.0 m.
  **97 % of > 0.5 m steps are STICKY** (5-epoch follow-up: the level persists) —
  re-realizations, not spikes.
- **The churn is uncompensated on the wire**: corr(ΔCLK, ΔPB) = −0.001…−0.008 per
  band; the P channel is quasi-static (20–104 mid-arc steps > 0.1 m per band per
  12 h vs ~18–23 k for the clock — PB has `NetworkPbEmitFilter`, the clock has
  nothing); the consumer-facing phase total (c0 + pb) churns exactly as much as c0
  alone; corr(ΔCLK, ΔCB) ≈ +0.14 (small).  ⇒ Removing clock-channel churn at emission
  removes NOISE, not a re-split trade.

## §2 Falsified at design time — per-key value smoothing (do NOT rebuild)

Offline prototype of a per-sat scalar-KF continuity filter (`NetworkPbEmitFilter`
mechanics transplanted to the clock channel) against the stored 12 h product
(scratchpad `clkfilter_sim2.py`, held-frame emission, v1-censored σ̂ clamped to
[0.5, 3]×seed):

| arm | resets(=marks)/12 h | per-sat reset spacing | key offset vs solve |
|---|---|---|---|
| k=4, q=(5 mm)²/30 s | 3218 (268/h) | med 4 min | p99 0.57 m |
| k=5, q=(1 cm)²/30 s, seed 10 cm | 872 (73/h) | med 15.5 min | p99 1.40 m |
| k=6 + confirm-M2 (v2 censoring-free feed) | 35 | — | **max 30.8 m — diverges** |

The per-sat channel is a **walk** (reset |e| med 0.70 m at ~20-epoch spacing =
σ_w·√N exactly), so any hold+gate either storms marks (tight gate) or walks off
the solve's self-consistent frame (loose gate; the v2 censoring-free σ̂ feed
additionally spirals open under ignition contamination).  Confirm-M windows only
add latency (97 % of gate events are sticky).  **Per-sat temporal stiffness is
estimator work (Q3's guarded temporal anchor), not emission work** — the same
verdict FC-1/C1 reached from the prediction side, now from the smoothing side.

## §3 Design (what IS built)

One leaf, `NetworkClockEmitRebase` (`include/products/`, precedent
`NetworkEmitRebase`/`NetworkPbEmitFilter`), called from `networkEmitStore` after
the PB-filter block, BEFORE `emitter.emitToStore` — baked into the store push and
the on-disk product, so a `NetworkProductIO` replay reproduces everything with no
re-run (marks land as ordinary E/R records the replay already consumes).
Outputs-only: the solve is byte-identical even when ON.

**Stage 1 — common-mode frame hold** (`network.emit_clock_frame_hold`, default
OFF).  Per constellation: μ = median innovation of this epoch's raw c0 against the
per-sat previous EMITTED value, over ≥ 4 fresh (≤ `max_gap_s` = 600 s), unforced
keys; A_c += μ; **emit every satellite in the held frame** (c0 − A_c — new sats
too, so between-sat differences are preserved exactly; A_c never re-enters the
wire).  The removed direction is the per-epoch clock-ensemble gauge (a consumer's
per-epoch receiver clock absorbs it identically) ⇒ information-free by
construction; prototype: census common channel **613 → 0**.  Guards
(invariant #1/#7):
  - **Leash**: |A_c| > `network.clock_frame_leash_m` (default 50.0; measured 12 h
    wander 23 m) ⇒ re-anchor to the solve frame (A_c = 0) **with**
    `markDatumChange({const})` — the re-coupling edge is marked, never silent.
  - **Resume seam**: a warm-resumed run (n_emitted restored > 0) cold-starts the
    carry ⇒ first armed epoch re-anchors WITH the datum mark (F1 seam rule: a warm
    resume is an event).
  - A pending per-sat forced bump (`retro_emit_sats`) or a datum-changed epoch
    excludes those keys from the μ vote; the datum step itself is absorbed like any
    common innovation — the upstream mark already tells the consumer.

**Stage 2 — per-sat step marks** (`network.clock_disc_jump_test`, default OFF;
independent of stage 1 — alone it marks the raw wire).  After the frame hold:
|Δc0_held(sat) − μ| > `network.clock_disc_jump_m` (default **1.5 m** ≈ 16 σ_w;
empirical §1: ~7 marks/h on this lineage, where the census's 0.5 m floor would be
73/h) ⇒ `emitter.markSatDiscontinuity({sat})` — the B1 retro-bump path: R record +
phase/code/clock counters step, the exact chain Q4 proved end-to-end at the
consumer (both-band ambiguity reset at the event epoch).  Skipped for
init/gap/forced keys.  This respects RTA-11: the emitter's clock counter still has
no internal magnitude test — the test lives at the emission-prep tier and drives
the FORCED path, which replay reproduces from the serialized R/E records.

**Telemetry** (invariant #4): armed-only run-end line (epochs, rows re-based,
Σ|μ| drained, end/max |A_c|, marks, re-anchors, inits, gaps) via
`networkTeardown`; per-event `trace_events` rows (`CLK_FRAME`/`reanchor`,
`CLK_STEP`/`mark`).  Fingerprint class: outputs-only ⇒ fingerprint-SAFE (no
solve-fingerprint token; legal on a warm resume, which re-anchors marked).

## §4 Pre-registered gates

- **G0 OFF-path byte-identity**: `_golden63_gps_12h_internal.json` re-run unchanged
  ⇒ products.txt record stream byte-identical to the stored run (F6-gate-1 sense;
  no legend change).  Doubles as the same-build 12 h control.
- **G1 20-ep ON smoke** (both knobs): stage 1 applied every epoch, |μ| ≈ the
  control's common-step series; 0-to-few marks; solve-side traces byte-identical
  to control; counters live.
- **G2 1 h ON**: census common channel 0 epochs (control ~½ of epochs); per-sat dev
  distribution unchanged; marks ≈ Poisson(7/h); consumer replay of both products
  unchanged or better.
- **G3 12 h A/B (the verdict)**, ON vs the G0 control: `product_step_census.py`
  clock channel 613 → ≤ 5 common, outlier epochs carried by R marks (census "wire
  marks" line non-empty); WTZZ consumer (live closed loop + replay) hourly medians
  / resets; AR fix rate vs 0/1440.  **Pre-registered expectation: AR does NOT
  recover from this card** — the AR blocker is the per-sat walk (§2), which is Q3
  estimator work.  This card claims the census, the wire-honesty (marks), and
  consumer unchanged-or-better.

## §5 Results

Session binary `gnss_processor_clkr.exe` md5 `33613AB60D9C6525FC1C11BE03CE2E8A`
(Release, 2026-08-09).  Unit suite 1317 passed / 1 skipped (fixture-gated) — +12
cases in `tests/test_clock_emit_rebase.cpp` (exact common-mode removal incl. a
17.7 m-scale spike, SD preservation, held-frame first sighting, min_common freeze,
leash re-anchor with reported constellation and suppressed step test, mark
passthrough never laundered, raw-wire marks with hold off, forced-key exclusion,
median robustness, beyond-horizon first sighting; loader: defaults off,
independent knobs).

**G1 (`runs/clkr_20ep_{ctl,on}`) — ALL PASS.**  P1 counters live: 20 epochs /
640 rows, Σ|μ| 1.96 m drained, max |A_c| 1.42 m, 0 re-anchors, 3 marks, 32 inits
(control console: zero CLK-R lines).  P2 outputs-only proven at the wire: the
ENTIRE non-C/R record stream (5351 E/B/P/H/S lines) byte-identical.  P3 SD
preservation: worst |SD_on − SD_ctl| = 3.6e-15 m; per-epoch shift
constellation-uniform to 1.8e-15 m.  P4: 3 marks = 3 R records (control 0).

**G2 (`runs/clkr_1h_on`) — PASS.**  Census clock channel **0 common-mode step
epochs** over the hour (stored control class ~60/h in hour 1); per-sat outlier
epochs 27 (unchanged, as pre-registered).  Run-end: 121 epochs, Σ|μ| 12.95 m,
max |A_c| 9.24 m (leash 50), 0 re-anchors; 3 marks = 3 R records = 3 `CLK_STEP`
rows (all G07, TOW 259230–259440, ignition steps −2.1 / −1.5 / −2.1 m; event ↔ R ↔
counter closes 3/3/3, invariant #4).

**G3 (`runs/clkr_12h_{ctl,on}` vs the stored denominator) — census and
byte-identity gates PASS; the live-consumer grade required a decomposition.**

- **G0 PASS, maximally**: the ctl re-run's products.txt is **byte-identical to
  `runs/golden63_12h_internal/products.txt`, whole file**, and its live consumer
  reproduces the stored hourly medians exactly — OFF-path byte-identity + rebuild
  determinism at 12 h scale in one gate.
- **P2 PASS**: ON vs ctl non-C/R record stream byte-identical (374,577 lines).
- **P1 PASS — the headline**: census clock common-mode step epochs **613 → 1, and
  the 1 is the pivot handover itself** (tow 276150, the run's one `datum_changed`
  mark, G:G01→G02; all keys upstream-forced ⇒ empty μ vote ⇒ the real datum step
  passes through, marked).  Wire discontinuities and wire marks now COINCIDE.  The
  next epoch shows the ctl wire still relaxing (−0.24 m) while the held wire
  re-absorbs the tail (+0.000).  Per-sat dev bit-comparable to ctl
  (6.2/21.2/67.9 cm).  Marks: 68 (sat,epoch) events on 42 R lines (multi-sat R
  grammar); Σ|μ| drained 142.03 m; max |A_c| 40.92 m (leash 50 — thin margin);
  0 re-anchors; 8 gap re-inits.  ⚠ max |A_c| ran 41 m against the measured 23 m
  wander because A rides the handover spike's neighbourhood too — **size the leash
  off max|A|, not the end-of-day wander.**
- **P5 as pre-registered**: AR 0/1440 in every arm (blocker = the per-sat walk, §2).
- **Replay decomposition (ctl / ON / ON-with-R-stripped, byte-exact twin
  pattern)**: the R-stripped twin reproduces the ctl consumer's reaction profile
  exactly (PB resets 82/82, clock-disc 10/10, CB-disc 532/532; ON: 118/26/570) and
  its positions are identical to ctl through h0–h1 — **the frame hold is
  consumer-reaction-neutral by measurement**, and the full card's consumer deltas
  are the marks' resets.
- **P4, resolved by a live one-knob bisection** (`runs/clkr_12h_hold` = stage 1
  only).  The full-ON arm's LIVE consumer took a late-day tail regression (p95
  14.6 → 21.3 m, epochs > 20 m: 1 → 106, expressed as a sustained +5–10 m UP bias in
  two windows, one of them mark-free) that the replay consumer did not reproduce; the
  bisection attributes it entirely to the MARKS:

  | live arm | med | p95 | max | >20 m |
  |---|---|---|---|---|
  | ctl | 9.08 | 14.57 | 20.66 | 1 |
  | **hold only** | **8.35** | 14.93 | **17.91** | **0** |
  | hold + marks | 9.28 | 21.29 | 23.53 | 106 |

  Hold-only is IDENTICAL to ctl through h0–h3 (3.1/7.4/9.2/11.4 — the gauge
  cancellation, measured), then scatters both directions with a net favourable
  tail (worst epoch 20.7 → 17.9 m).  Its wire census is the same
  613 → 1-marked-handover; counters identical
  (Σ|μ| 142.03 m, max |A_c| 40.92, 0 re-anchors).  The 68 mark-driven whole-sat
  resets are pure transient cost on a quiet day: the marked population is ~98 %
  estimator churn (FC-1), and resetting a converged float consumer on churn pays
  the transient without buying information — the Edge-1/F13 shape (mark VALUE needs
  an onset-enrichment / event-day grade).

## §6 Verdict and standing

- **Stage 1 — `network.emit_clock_frame_hold` — VALIDATED on every pre-registered
  gate**: census common-mode class 613 → 1 and the survivor is the marked pivot
  handover; outputs-only proven byte-level at 12 h; between-sat differences
  preserved to rounding; consumer reaction-identical (replay) and
  unchanged-to-slightly-better (live: med −0.7 m, tail improved, 0 > 20 m epochs).
  Default-OFF per the no-promotion rule; recommended for the internal-lineage
  golden configs at the next promotion pass (fingerprint-SAFE, solve-untouched).
  For > 24 h lineages size the leash off max |A_c| (41 m/12 h), not the end-of-day
  wander (23 m).
- **Stage 2 — `network.clock_disc_jump_test` — mechanism VALIDATED, value NOT
  graduated**: 68 events tie out end-to-end (event ↔ R record ↔ counters ↔
  +36/+16/+38 consumer reactions) and land at genuine re-realizations (16 σ over
  the walk) — but on a quiet day the live consumer pays 106 > 20 m transient
  epochs.  Keep default-OFF; arm on event-day / onset-enrichment studies where the
  marked class carries real faults (the §1.4 enrichment table remains the success
  criterion for any future arming).
- **AR fix rate 0/1440 in every arm, exactly as pre-registered** — the blocker is
  the per-sat clock walk (σ_w ≈ 0.09–0.16 m / 30 s vs ~2 cm physical), which no
  emission-side processing can honestly remove (§2).  The AR path runs through Q3
  estimator-side clock stiffness.
- Wire-honesty net effect: an ordinary day's product now carries ~0 unmarked clock
  discontinuities (was 613), 1 marked datum event, and (optionally) a declared
  per-sat re-realization class.

## §7 Theory placement — the S-basis realization contract

The Odijk et al. (2016) comparison prompted by this card is formalized in
**`docs/SPEC_sbasis_realization_contract.md`**: the producer's realized S-basis in
the paper's Table-2 notation (code-verified), the estimable interpretation of the
emitted C/B/P/iono records (Table-3 analogue, incl. the measured DEFECT-2 Table-8
lumping), and the **realization-transfer contract** — which per-epoch re-occurring
gauge is HELD by which emission-side S-transformation (NetworkEmitRebase / Card 2 /
CLK-R stage 1) and MARKED on genuine change.  In that frame this card is the
clock-gauge row: the wire churn was solutions shipped from a per-epoch-changing
S-system without the connecting S-transformation (the Zhong-2015 artifact class),
and the frame hold is that transformation applied at emission.  The two residues
the contract cannot fix — the per-sat clock walk (Q3) and the iono⊕rx-DCB level
(authority; item 0b) — are named there with the theory's own localization.
