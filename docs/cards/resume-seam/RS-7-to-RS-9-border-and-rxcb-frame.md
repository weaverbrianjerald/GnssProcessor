# RS-7 … RS-9: the dense border (KEEP_OFF) and the rx-CB inject gate's frozen frame

**STATUS**

- **id:** RS-7 … RS-9
- **topic:** resume-seam
- **status:** closed
- **summary:** The dense-border restore is exact but not the residual (KEEP_OFF); the flat rx-CB withhold was a re-seeded linearization point, and RS-9's restore cuts it 26.3 % to 9.8 % (RECOMMEND_SCOPED).
- **dates in the text:** 2026-09-13
- **knobs / posture:** `network.resume_border_restore` (RS-7, KEEP_OFF on the field; the decision names a re-test once the station-layer residual is closed), `network.state_snapshot_rxcb_lin_capture` + `network.resume_rxcb_lin_restore` (RS-9, RECOMMEND_SCOPED), `network.state_snapshot_signal_capture` + `network.resume_signal_canon_restore` (RS-8, pre-registered here, results in the next card), `network.clock_anchor` (not armed, so the first-fold trip counter is vacuous)
- **hashes named in the text** (commit / exe / byte-cert): `87a33bf0`, `0a7efa8`, `c45e52a0`, `3b6459f`, `545b8bdd2b638231`; exe `23C628B3`
- **run dirs named in the text:** `runs/evseed_field6h_v6`
- **source:** `docs/archive/SPEC_event_harness.md` §21.16, §21.17, §21.18, §21.19

**Outcome.** What holds the seam residual RS-6a left? RS-7 restores the dense border exactly, yet every P9b bar FAILS and the code-bias level worsens (0.460 to 0.804 m), so RS-6a's attribution is FALSIFIED. RS-9 restores the first-epoch `x_lin` that a resume re-seeds for the rx-CB gate: R1 PASS as graded (the share sits 1.2 points below the seed's 11.0 %, outside the ±1 band, favourably), R2 FAIL on every pooled bar (those bars RETIRED as divergence measures). Continued in [RS-8 … RS-10b](RS-8-to-RS-10b-latch-and-close-out.md); the gate's frame-blindness on a continuous run stays [open](../../status/OPEN_ITEMS.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 21.16 RS-8 pre-registered — the six L2X families' "datum drop" is the per-station CANONICAL-SIGNAL latch re-deciding at the resume epoch, not a datum memory (2026-09-13 ~06:40 local; read-only investigation on the existing legs, manager pre-registration BEFORE any leg is read)

**The finding (no new run; Opus investigation on `evseed_field6h_v5` / `v5raw` and the RS-6 legs).**  §21.10–§21.14 graded
raw phase bias "ex L2X" because the six GPS L2X families step ~25 m at +1 and their eq34 component datum
receiver GAMB00PYF "vanishes" (→ CPVG00CPV).  The datum memory is complete and IS restored: the ep0604 snapshot
carries `EQD,G|L2X,GAMB00PYF`, eleven `EQS,G*_L2X,GAMB00PYF` roots, the `PA` pins and the parent edges
INEG00MEX←G31 / OAFA00ARG←G27, and the leg's RS-3 line restores all of it (6 sticky receivers, 176 parent edges,
126 roots, 302 pin targets).  The seed does not re-elect at the seam (its PHASE_DATUM rows at 277290–277350 carry
zero L2X; its L2X tracker set is `{GAMB, INEG, OAFA}` for all 720 epochs).  The leg's election falls through the
sticky and incumbent tests (`NetworkChainDatum.cpp:306–353`, then the reref stamp at `:507–510`) for one reason:
GAMB00PYF is not in the L2X `rx_list` because it contributes **no L2X arc** — its L2X tracker set at +1 is
`CPVG|G10 G12 G15 G23 G24 G25 G29 G32, INEG|G10 G28 G31 G32` (12 arcs) against the seed's
`GAMB|G04 G08 G09 G27 G31, INEG|…, OAFA|…` (15 arcs); the unions are exactly the emitted L2X key sets (seed 11
satellites, leg 10, intersection = the six that re-reference; the other five have no L2X column at all ⇒ "None").

The cause is `PppModel::decideConstellationCanonicals` (`src/core/model/PppModel.cpp:414`, guard `:427`, lock
`:479`; member `include/model/PppModel.hpp:513–523`): per station, on that station's FIRST processed epoch, it picks
the per-(band, obs-type) signal with the highest satellite count and locks it for the run.  The latch is in no
snapshot and its "Canonical signals locked for GPS …" line goes to the `ppp_model` logger, which is 0 bytes in every
run — the swap is silent.  RINEX tallies at the two run starts reproduce every observed tracker set:

| station | 00:00 (seed ep 1) | 05:02 (leg ep 1) | seed canonical | leg canonical |
|---|---|---|---|---|
| GAMB00PYF | L2W 9 / **L2X 10** | **L2W 10** / L2X 8 | L2X | **L2W** |
| CPVG00CPV | **L2W 10** / L2X 7 | L2W 8 / **L2X 11** | L2W | **L2X** |
| OAFA00ARG | L2W 7 / **L2X 8** | **L2W 9** / L2X 8 | L2X | **L2W** |
| INEG00MEX | L2W 1 / **L2X 7** | L2W 2 / **L2X 7** | L2X | L2X |

On the matched raw pair (`v5raw` vs `rs6_raw_both`) the six L2X keys read p50 |Δ| **25.6 m** at +1 against 1.50 m
for the other 123 — §21.10's 24.7 m to the digit.  (A latch that decides a station's canonical L2 signal from ONE
epoch's satellite count is a design smell in its own right — GAMB's L2W/L2X counts differ by one or two satellites
at both instants — but that is not this card: the continuity defect is that a resume RE-decides it.)

**Build (RS-8; the RS-3/RS-5/RS-6 pattern — default-OFF, byte-identical when off, forward-only, NOT fingerprinted).**
Write knob `network.state_snapshot_signal_capture`: one row `CN,<station index>,<constellation>,<band>,<C|L>,<signal>`
per locked canonical (from `PppModel::exportCanonicalSignals()` over `constellation_canonical_` +
`_decided_`), format-forward under the existing header; the field golden pins it (write side only, products
byte-identical) and `event_leg.py write-seed` arms it.  Read knob `network.resume_signal_canon_restore` (requires
`resume_frame_restore` — the arcs the restored realization frame names are the writer's canonical signals'):
at a warm resume, after the RS-3 datum restore, restore each station's map (`PppModel::restoreCanonicalSignals()`)
so the `:427` guard treats the station as decided; a snapshot without `CN` rows is inert and WARNED (the rider that
makes this defect visible from now on: the "Canonical signals locked" line is re-emitted on `main_log` at every
decision AND every restore, with the counts); counters + run-end line.  Two leg-side diagnostics are NOT wanted.
Seeds: `runs/evseed_field6h_v6` and `v6raw` = the v5 recipes + the write knob (their products must be byte-identical
to v5's / v5raw's: 0 differing values — the RS-5→RS-6 precedent), ep0604 at the same tow, fingerprint unchanged.

**Gates, frozen (legs `rs8_raw_ctl` / `rs8_raw_canon` from `v6raw` ep0604 and `rs8_null_ctl` / `rs8_null_canon`
from `v6`, 60 epochs, one exe; every other RS restore at the harness default; `resume_coupling_restore: false`):**
* **Q1 the tracker set:** at +1 the leg's L2X tracker set in `network_if_slip.csv` is the seed's `{GAMB00PYF,
  INEG00MEX, OAFA00ARG}` and the emitted L2X satellite set is the seed's 11; every station's canonical map at +1
  equals the writer's (the restore line counts 0 disagreements).
* **Q2 the datum:** the first post-resume epoch's eq34 re-references drop from 8 to ≤ 2 (the seed's own two L5Q
  re-references at 277320), with zero L2X rows; the RS-3 restore line unchanged.
* **Q3 the level:** on the raw pair the six L2X keys' |Δ| at +1 p50 ≤ 2.0 m (from 25.6), i.e. at the other keys'
  level, so raw PB at +1 is graded over all 129 keys from here on; the other 123 keys within 0.05 m of the control
  leg's (the restore touches L2X only).
* **Q4 byte-identity:** cold smoke knob absent vs armed `products.txt` identical (`87a33bf0…` on the field golden);
  `rs8_raw_ctl` reproduces `evleg_rs6_raw_both`'s `products.txt` byte-for-byte (same posture, new binary — the
  resume path certified too); `v6` / `v6raw` products byte-identical to `v5` / `v5raw`; suite green.
* **Q5 the rider:** the canonical line appears on `main_log` for every station on the cold seed and, on the legs,
  reads "restored" (canon) / "decided" (ctl); a resume against a `CN`-less snapshot (a v5 seed) warns once.

### 21.17 RESULTS — RS-7: the dense border restores exactly, and it is NOT the residual (KEEP_OFF with the mechanism); RS-7b census pre-registered (2026-09-13 ~08:20 local; worker on worktree-agent-abc0bf1feef082b22 `0a7efa8`, merged; manager grade)

**Build.**  `network.resume_border_restore` (default false; throws without `resume_frame_restore`; warn-DISARMS
without `shared_carry_full_cov`; does NOT require Card 1f or `carry_conditional_coupling` — both locked by tests);
the §18.12 (2a) dense-border block gated on either knob and naming the knob that armed it; the C1 seam line made
truthful (the "diagonal carries" clause prints only when no dense restore follows); the first-fold diagonal census
armed on EVERY warm resume with the dense carry on (control legs read it too); run-end `Network (RS-7 border
restore)` line + a plain-warm-resume readout; `tests/test_resume_border_restore.cpp` (10).  Cold smoke `87a33bf0…`
knob absent AND armed; Debug suite 2023 / 276 suites / 2022 passed / 1 skip / 0 failed; exe `23C628B3…`.
The border under test: the 150 QK/QP keys are **29 CLK + 121 CB** (C1W 29, C2W 30, C1C 30, C5X 16, C5Q 14, C2X 2)
— no PhaseBias keys on this lineage (§project_network_fullcov_carry's regime caveat, confirmed) — and all 150 of
the 256 D rows' non-VtecSh keys restore (0 dropped).  `rs7_null_ctl` `products.txt` = `c45e52a0…`, §21.15's
certified hash, on a third independently linked binary.

| leg − seed (raw = v5raw, estimator; null = v5, wire) | raw ctl | raw border | null ctl | null border |
|---|---|---|---|---|
| C1 "diagonal carries" clause / RS-7 keys restored | YES / – | **no / 150 (0 dropped)** | YES / – | no / 150 |
| border keys folded DIAGONALLY at the first fold | 150 | **0** | 150 | 0 |
| raw PB +1 p50 (p95) ex L2X | 1.496 (2.825) | 1.457 (2.629) | 0.000 (0.002) | 0.000 (0.002) |
| receiver clocks +1 p50 (p95), 61 sta | 0.934 (5.70) | **1.124** (5.82) | idem | idem |
| SH a₀₀ +1 / +6 (TECU) | +0.43 / +0.11 | +0.45 / +0.51 | idem | idem |
| rx-CB gate withheld share (seed, same window: **11.01 %** = 403/3660) | **26.31 %** | **26.28 %** | idem | idem |
| W6 code bias +2 / pooled p50 | 0.152 / 0.460 | 0.140 / **0.804** | idem | idem |
| clock contrast +2 / pooled p50 (p95) | 0.291 / 1.651 (9.11) | 0.361 / 1.563 (**7.32**) | idem | idem |
| ZWD pooled p50 / p95 | 0.659 / 5.03 | 0.637 / 5.03 | idem | idem |
| D5 flags +1; births +1 / +2; eq34 rerefs | 38; 64 / 7; 64 | 39; 64 / 7; 64 | idem | idem |
| PB wire p50 +1 / +2 / +30 | – | – | 0.0004 / 0.0007 / 0.048 | 0.0005 / 0.0011 / 0.035 |
| stateful consumer at the seam: PB / code / clock re-refs (±30 s) | – | – | 0 / 22 / 0 | 0 / **25** / 0 |

**P9a PASS** — the border is dense from the first fold, 150/0, the diagonal count 150 → 0, zero dense-fold
fallbacks.  ⚠ Its second instrument is vacuous here: the §18.11 "first-fold clock trips" counter is the Card-Q3
decouple total and `network.clock_anchor` is not armed on the field golden, so it reads 0 structurally (the run-end
line now says so instead of letting the 0 read as health).  **P9b FAIL on every bar, with the border demonstrably
dense** — so §21.14's attribution is FALSIFIED.  What the border moves: clock contrast p95 9.11 → 7.32 (at +60
2.85 → 1.87), raw PB p95 2.83 → 2.63, ZWD 0.659 → 0.637.  What it moves the wrong way: the W6 code-bias level
0.460 → **0.804** pooled (every signal: C1C 0.29 → 0.48, C1W 0.88 → 1.07, C2W 0.47 → 0.78, C5Q 0.37 → 0.79, C5X
0.33 → 0.77), receiver clocks at +1 0.93 → 1.12 m, consumer code re-references 22 → 25.  The code-bias trajectory
INVERTS: the border arm is better at the seam (+1 0.070 vs 0.086, +2 0.140 vs 0.152) and then walks ~1.7× faster
(+10 0.495 vs 0.333, +30 0.844 vs 0.440, +60 1.139 vs 0.666) — restoring the writer's cross-covariance fixes the
first fold and then stiffens the code-bias block against a station layer that is NOT the writer's (invariant #1's
shape: a stiff prior across a state the run does not share).  What does not move at all: the rx-CB gate share,
**flat at 26 % on every epoch of both arms** against the seed's flat 10–11 % over the identical window.  **P9c
PASS** (PB wire 0.000 unchanged; a₀₀ Δ 0.02 TECU); the `zwd_seam_reseed` pair on the border posture reproduces
§21.14's mixed pattern unchanged (ZWD 0.637 → 0.430, clock contrast 1.563 → 1.168, rx clocks 1.12 → 0.82; CB 0.804
→ 0.918, a₀₀ +0.45 → +0.74 and no decay, D5 39 → 53), so that knob's field verdict does not change either.

**Decision.**  `network.resume_border_restore` = **KEEP_OFF on the field** with the mechanism above (the knob is
correct, tested, default-OFF and stays in the tree; re-test it after the station-layer residual is closed — the
sim's leg-E win (§18.14) was measured WITH the coupling rows and with a station layer that continued).  Not armed
by the harness; no golden change.  Harness notes: `ev_launch.ps1` wrote an empty exit-code line (`$p.ExitCode`
unpopulated; fixed by touching `$p.Handle`); a `CMakeLists.txt` edit landing during a configure leaves `build.ninja`
newer than it and the new test file silently uncompiled — count suites, touch `CMakeLists.txt`.

**RS-7b pre-registered — a CENSUS, no new run: WHERE the flat 26 % lives.**  Everything §21.14 called "the
residual" (rx-CB withheld share 26 % vs 11 %, code-bias level walk, ZWD 0.64 m, pooled clock contrast 1.5 m, 22
consumer code re-references) is persistent over the whole leg, so it is a per-epoch structural difference between
the leg's and the seed's state, not a transient.  Read from the existing `rs7_raw_{ctl,border}` legs and `v5raw`
over the identical window (epochs 605–664): (a) the rx-CB withholds per (station, code key, epoch) with the
innovation's magnitude, sign and σ — concentrated on a station/signal subset withholding EVERY epoch (a per-station
split) or diffuse (a gate calibration); constant per-key offset (a frame) or scatter (instability); which term of
"10σ + 0.5 m" fails; (b) what the inject innovation is measured AGAINST in the code (`NetworkRunContext.cpp` ~5231,
`RxCbInjectAction::SkipInnov`) — the writer's `x_lin`, the previous posterior, or the epoch's code-only estimate —
and what a withhold does to the carry (the fingerprint note at ~214 says this gate reads innovations against
`x_lin`, and an `x_lin` shift is a solve no-op that a screen still sees, SPEC_lane_slip_response §7.2); (c) receiver
clocks and ZWD per station, leg − seed as 60-epoch series: persistent per-station offsets, and whether they sit on
the withholding stations; (d) W6 code bias per (sat, signal) and clock contrast per satellite, leg − seed series:
common-mode per signal (a datum realization) or per satellite, and whether clock and code bias re-split per
satellite (CLK + CB per (sat, signal) leg − seed — the consumer-invariant sum — against each alone).  Candidate
mechanisms to weigh, none assumed: a station-side `x_lin` re-seeded at the resume that the gate reads (H1); a
per-satellite clock↔code-bias re-split invisible to the consumer (then the instruments overstate the residual and
the 22 re-references are its whole cost) (H2); a station subset with a persistently different clock/CB/ZWD split —
the field's ZWD datum sink (H3); a restored carry variance so tight that 10σ sits below the leg's own scatter (H4).
Deliverable: the mechanism with the evidence, the smallest increment, and its harness grade.

### 21.18 RESULTS — RS-7b census: the flat 26 % is the rx-CB inject gate reading a FROZEN, un-snapshotted linearization point re-seeded at the resume; RS-9 pre-registered (2026-09-13 ~08:50 local; read-only Opus census on the RS-7 legs, script `scratchpad/rs7b_census.py`; manager pre-registration BEFORE any leg is read)

**(a) Where the 26 % lives.**  One rx-CB key per station (`GPS_DCB`, 61 per epoch), so "by key" is "by station".
Over the identical window (tow 277320–279090, k = 1..60): seed 403/3660 = **11.01 %**, `rs7_raw_ctl` 963/3660 =
26.31 %, `rs7_raw_border` 962/3660, `rs6_raw_ctl` (no station-layer restore at all) 942/3599 = 26.17 %.  Per-epoch
counts are flat and deterministic — seed `6 6 6 … 7 7 7` (one step at k = 14), leg `17 17 … 16 16` — and the set is
**17 stations withholding 45–58 of 60 epochs** (KRGG 58, GUAM 57, SGOC 56, CAS1 55, GAMB 55, SOLO 54, OAFA 54, BOR1
54, KOKV 53, MAJU 52, LCK4 51, MRO1 51, COCO 50, CUSV 49, GOPE 49, LMMF 48, HKSL 45) against the seed's **7** (KOKV
57, CPVG 56, MRO1 54, LMMF 53, GOPE 52, GAMB 49, LCK4 46): 6 shared, 11 leg-only, 1 seed-only.  Concentrated,
per-station, fixed from k = 1.  **No guard term fails — the innovation does.**  `rxCbInjectGate`
(`include/filter/NetworkRunTypes.hpp:1535–1549`) withholds on `innov² > k²·(var_eff + guard_var)`, k = 10,
guard_var = 0.25 m², `var_eff = var + q·Δt` with q = 1e-9: measured `var_eff` p50 1.30e-2 m², so the 0.5 m guard is
95 % of the σ² and the bar is a fixed **5.13 m** — on a quantity whose epoch-to-epoch motion is 9.4e-4 m p50.  H4
(gate calibration) is dead: `var_eff` would have to grow 400× to move the bar 20 %.

**(b) What the innovation is measured against — the proof.**  `ApplicationNetworkAssembly.cpp:3954`:
`cb_innov_raw = value_abs − x_lin(cb_idx)`; `x_lin` is the station StateManager's code-bias state, seeded ONCE by
`StateInitializer::initializeCodeBiases` (`StateInitializer.cpp:656`, the median over satellites of a code-residual
DCB, written at `:749/:754`) on the station's FIRST processed epoch behind the one-shot `isCodeBiasInitialized()`
(`MeasurementModel.cpp:1799–1802`), never written back; `rx_cb_carry_demean` / `_harvest_demean` /
`rx_cb_carry_step_gate` are all off here, so the raw innovation is the tested one.  At k = 1 the leg has restored
all 61 RC entries, so `value_abs`, `var` and `last_seen_s` are bit-identical to the seed's at the same tow, the gap is
30 s in both, k / q / guard identical — same numerator, same threshold, **6 withheld in the seed, 17 in the leg**: the
only input left is `x_lin`.  `rs6_raw_ctl` (carry NOT restored) trips the same ~16 from k = 2 — the set is a property
of the leg's `x_lin`, not of the carry's provenance.  `x_lin` is in no snapshot row (RC = `value_abs / var /
last_seen_s`, `NetworkStateSnapshot.cpp:257–259 / :686`) and nothing restores it.  **What a withhold does:** the fold
is skipped (`:4006–4009`, zero force) but the harvest overwrites the entry unconditionally
(`NetworkRunContext.cpp:5328`) and the prune only drops > 900 s — a withholding station is refreshed and refused
every epoch, forever, with no rx-CB memory, while the run-end line reads "61 carried" (invariant #4 blind spot).
Its carry variance is 1.3e-2 m² against a folding station's 3e-5.

**(c)(d) The consequences sit on the withholding stations, and the clock error is real to the consumer.**  Leg −
seed with the per-epoch across-station median removed (invariant #5): the 18 withholding stations carry CB
`value_abs` p50 **0.510 m** vs 0.019 on the 44 folding ones (26×, essentially exclusive), receiver clock 1.94 vs 1.03,
ZWD 0.95 vs 0.62 (worst SOLO CLK 16.8 / ZWD 7.8, CPVG 8.2 / 4.1, CAS1 5.3 / 3.5).  H3 is inverted — the per-station
split is the CONSEQUENCE of the withhold (the set is fixed at k = 1 before any split can develop).  Products leg −
seed over 60 epochs: clock contrast p50 1.497 (p95 8.99), code bias 0.427, and the consumer-invariant sums |CLK + CB|
1.566 / |CLK − CB| 1.397 against |CLK| 1.497 alone, per-satellite corr(ΔCLK, ΔCB) = +0.59 (a re-split would be ≈ −1):
**H2 falsified**, the 22 code re-references are not the whole cost.  Structure: per-signal means ≈ 0 (only C2X +0.378,
the §21.16 L2X key) — not a datum realization; a per-(sat, signal) CONSTANT explains 85.9 % of the ΔCB variance (per
satellite alone 29 %) — a frozen level offset per OSB key, the "level walk" shape; clock contrast per satellite
persistent (G16 +9.2 ± 5.2, G21 −8.6 ± 2.8).

**Mechanism (H1).**  RS-6 restored the rx-CB carry's numerator and left its reference cold: `x_lin(cb)` is a frozen,
arbitrary linearization point re-derived at the leg's first epoch from one epoch of code at 05:02 instead of 00:00;
the gate's only statistic is the distance from it, so 11 extra stations sit outside a 5.13 m bar for the whole leg,
lose their receiver-DCB memory permanently, and the receiver-DCB / satellite-OSB / clock / ZWD split re-realizes on
them.  Invariant #7 exactly: the re-coupling edge re-validated the value it reconnected but not the frame it is
judged against — RS-8's defect family (a per-station, first-epoch, un-snapshotted, silent decision; GAMB / OAFA /
CPVG sit in both sets, `resolveReceiverBias` feeds `initializeCodeBiases`).  ⚠ **An open defect in its own right,
NOT this card:** the gate is frame-blind on the continuous run too — 7 of 61 stations are refused for the whole 6 h
on the seed (no rx-CB memory all day).  The frame-invariant screen (`rx_cb_carry_step_gate`, LV-4) is the correct
statistic but was graded net-harmful as built (SPEC_level_information_budget §11 / §23.3: +64 / +35 / +16 % post-fit
phase rms, needs LV-5) — a solve-affecting card with the level-channel instruments, owed, not tonight.

**RS-9 pre-registered — snapshot and restore the code-bias linearization point (the RS-3/5/6 pattern).**  Write knob
`network.state_snapshot_rxcb_lin_capture` (requires `state_snapshot_station_capture`): row `RL,<station idx>,<cb
key>,<x_lin>` beside the RC row (`NetworkStateSnapshot.cpp:258`, read at `:686`), format-forward.  Read knob
`network.resume_rxcb_lin_restore` (requires `resume_station_restore` with its rxcb half): the RL map is stashed at
the RS-6 restore (`NetworkRunContext.cpp:1689–1707`) and consumed at the station's first-epoch code-bias
initialization (`StateInitializer::setCodeBiasSeedOverride` checked inside the `samples` loop at
`StateInitializer.cpp:745–753`, `markCodeBiasInitialized()` for covered keys; uncovered keys fall back to today's
seed and are counted); a snapshot without RL rows is inert and warned; counters + run-end line.  Solve-invariant by
construction under this posture (a code bias enters linearly, an `x_lin` shift is an exact solve no-op —
project_network_xlin_invariance; the only consumer is this gate; `rx_code_bias_datum`, whose RHS reads `x_lin`, is
off here — the loader refuses the pair).  Not fingerprinted (forward-only resume response policy).  Rider (every
run, trace-class): the rx-CB run-end line names the stations that withheld > 90 % of their epochs ("carried NOTHING")
so the seed's 7 become visible.  Rejected variants: seeding `x_lin := value_abs` (over-corrects the leg to ~0 % against
the seed's 11 % and breaks the leg↔seed comparability every RS gate uses); arming LV-4.  Seeds `v7` / `v7raw` = the
v5 recipes + the write knob (products byte-identical to v5 / v5raw), ep0604 at the same tow, fingerprint unchanged.

**Gates, frozen (legs `rs9_raw_ctl` / `rs9_raw_lin` from `v7raw`, `rs9_null_ctl` / `rs9_null_lin` from `v7`, 60
epochs, one exe, every other RS restore at the harness default; `resume_border_restore` off):**
* **R1 the closing bar:** rx-CB withheld share over k = 1..60 = the seed's 11.0 ± 1 % (from 26.3), and the leg's
  withholding set = the seed's 7 stations (KOKV, CPVG, MRO1, LMMF, GOPE, GAMB, LCK4) ± 1; restored RL entries 61,
  uncovered 0.
* **R2 the consequences, the RS-7 P9b bars re-used unchanged:** W6 code bias pooled p50 ≤ 0.10 m (from 0.46); ZWD
  pooled p50 ≤ 0.32 (from 0.66); clock contrast pooled p50 ≤ 0.5 (from 1.65); receiver clocks at +1 p50 ≤ 0.3 (from
  0.93); raw PB at +1 p50 ≤ 0.5 ex L2X (from 1.50; the L2X exclusion stays until RS-8 lands); the stateful consumer's
  code re-references at the seam ≤ 5 (from 22) with phase resets 0; PB wire 0.000 and a₀₀ within 2 TECU unchanged.
  Each bar reported PASS/FAIL; the card closes on R1 even if R2 leaves a remainder — R2's remainder then names the
  next memory.
* **R3 byte-identity:** cold smoke knob absent vs armed `87a33bf0…`; `v7` / `v7raw` products = `v5` / `v5raw`;
  `rs9_raw_ctl` `products.txt` = `evleg_rs7_raw_ctl`'s (same posture, new binary); suite green.
* **R4 the rider:** the seed's run-end line names its 7 permanently-refused stations; the lin leg names the same 7;
  the ctl leg names 17.

### 21.19 RESULTS — RS-9 closes the rx-CB frame (withheld share 26.3 % → 9.8 %, the seed's 11.0 %); the pooled 60-epoch leg − seed bars are RETIRED as seam instruments (they measure realization divergence); RS-10 pre-registered (2026-09-13 ~13:20 local; worker on worktree-agent-aac1e6222abdfe75f `3b6459f`+, merged; manager grade)

**Build.**  `RL,<station idx>,<cb key>,<x_lin>` rows paired 1:1 with the RC rows (61 on this lineage — the carried
`GPS_DCB`; the uncarried `GPS_IFB5` never reaches the gate) under `network.state_snapshot_rxcb_lin_capture`;
`network.resume_rxcb_lin_restore` stashes the map at the RS-6 restore and seeds each station's first-epoch code-bias
linearization point to the writer's value (`StateInitializer::resolveCodeBiasSeedOverride`, `markCodeBiasInitialized`
for covered keys); counters + run-end line; the rider on the always-on rx-CB line names the stations that withheld
> 90 % of their epochs ("carried NOTHING").  Cold smoke `87a33bf0…` knobs absent AND armed (the ON snapshot gains
exactly 61 RL rows); seeds `v7` / `v7raw` = the v5 recipes + the write knob, products byte-identical to `v5` /
`v5raw`, ep0604 at the same tow, fingerprint `545b8bdd2b638231`; `rs9_raw_ctl` / `rs9_null_ctl` `products.txt`
byte-identical to `rs7_raw_ctl` / `rs7_null_ctl`; Debug suite 2035 / 277 / 0 failed.  ⚠ The per-key "seeded from the
RESTORED linearization point" line goes to the `state_initializer` logger, 0 bytes on this posture (the §21.16
family); the counters are the instrument.

**R1 PASS — the frame was the mechanism.**  rx-CB withheld share over k = 1..60 (`scripts/rs9_census.py`, which
reproduces §21.18's 403/3660): seed **11.01 %**; ctl **26.31 %** (= `rs7_raw_ctl` to the row); **lin 9.78 %** — 94 % of
the 15.3-point gap closed, 1.2 points BELOW the seed (the ±1 band missed on the favourable side).  Withholding sets
(≥ 75 % of the window): seed 7, ctl 17, **lin 6** — every one of the 11 stations the defect created (KRGG GUAM SGOC
BOR1 SOLO MAJU COCO CUSV HKSL SCTB, CAS1 55 → 7) is back to 0/60; shared with the seed 5, only-leg 1 (OAFA00ARG),
only-seed 2 (CPVG00CPV, LCK400IND) — OAFA and CPVG are exactly §21.16's canonical-signal stations (RS-8, another
branch), LCK4 sits at 46/60 on the seed and 13/60 on the leg.  **R4 PASS:** the rider names 6 on the seeds (CPVG GAMB
GOPE KOKV LMMF MRO1 — LCK4 is 77 % over the window, under 90 % over 6 h), 16 on the ctl legs, 6 on the lin legs (GAMB
GOPE KOKV LMMF MRO1 OAFA).  Both arms restore identically at the RS-6 layer (61 rx-CB entries, 467 deviation
entries): the frame is the only difference.

**R2 — every pooled bar fails again, and the way it fails retires the bars.**  With 11 stations returned to folding:
W6 code bias pooled p50 0.460 → **0.545** (worse), clock contrast pooled 1.651 → 1.697 (worse; p95 9.11 → 8.93), ZWD
0.659 → 0.653 (flat), receiver clocks at +1 0.934 → 0.819 (12 % better), raw PB at +1 1.496 → 1.373 (8 % better), PB
wire 0.000 unchanged, a₀₀ +0.43 → +0.38, births / D5 / eq34 rerefs unchanged; the stateful consumer's code
re-references at the seam 22 → 24, phase resets 0 on both, cold-consumer settled 3D 11.79 vs 11.75 m.  The rx-CB
withheld population was therefore NOT what drives the pooled code-bias / clock / ZWD residual, and the RS-7 control's
own series says what does (`rs7_null_ctl/seam_grade.json`, leg − seed p50 by epoch):

| leg − seed p50 | +1 | +2 | +5 | +30 | +60 |
|---|---|---|---|---|---|
| clock contrast | 0.31 | 0.29 | 0.53 | 1.42 | 2.85 m |
| W6 code bias | 0.086 | 0.152 | 0.268 | 0.44 | 0.67 m |
| ZWD | 0.30 | 0.30 | 0.40 | 0.56 | 1.35 m |
| PB wire (DD-invariant) | 0.000 | 0.001 | 0.003 | 0.048 | 0.118 m |
| a₀₀ | +0.43 | +0.18 | +0.13 | −1.53 | −1.19 TECU |

Every shared channel — including the DD-invariant phase-bias wire and the SH a₀₀ — diverges roughly linearly from a
near-identical state on identical data: two realizations of a weak-datum, random-walk shared parameterization walking
apart as their gate decisions differ, the chain-datum churn the literature leaves unsolved (project_ppprtk_literature
_pack).  The 60-epoch pooled bars of §21.13/§21.14 (CB ≤ 0.10, clock contrast ≤ 0.5, ZWD ≤ 0.32) therefore graded
DIVERGENCE, not the seam — which is why RS-7 and RS-9 could each close their own mechanism exactly and leave those bars
untouched.  **From here the seam is graded at the SEAM EPOCH (+1 / +2 against the seed's own per-epoch step) and by
consumer instruments (the stateful splice's re-references at the seam; the cold-consumer replay), never by pooled
leg − seed levels over a window.**  On those instruments the seam's remaining cost is two items: the ZWD step at +1
(0.30 m p50 with `zwd_seam_reseed` ARMED, which should have pinned it to the writer's value) and the 22–24 code-bias
re-references the stateful consumer takes at the seam.

**Decision.**  Both RS-9 knobs RECOMMEND_SCOPED: the write knob pinned on both goldens (write side, byte-identical;
requires `state_snapshot_station_capture`, pinned), the read knob harness-armed on every leg.  `n_resume_rxcb_lin_
uncovered` counts only keys that have a carry (0 here).  The next harness seed (v8) carries CN + RL rows once RS-8
merges.

**RS-10 pre-registered — a CENSUS, no new run, on `rs9_null_lin` / `rs9_raw_lin` vs `v7` / `v7raw`.**  (a) The ZWD
seam step: per station at +1 and +2, the leg's ZWD vs the seed's, the snapshot's Z-row values vs the seed's ZWD trace
at ep0604/0605, and the leg's ZWD linearization point / a-priori vs the writer's (`zwd_seam_reseed` injects the Z rows
as B9 priors at q 1e-9 — in which frame, correction or absolute?); the same for the receiver clocks at +1 (0.82 m p50
— per-epoch white, so 0.82 m is the seam's per-station re-split at +1); is the 0.30 m the RS-9 family again (a
re-seeded `x_lin` for ZWD), a datum-sink restart, or the reseed acting in the wrong frame?  (b) The seam
re-references: which 22 (sat, signal) keys the consumer re-references, whether each is DECLARED (B-record disc
counter / code jump test) and its |Δ| at +1 leg − seed, against the seed's own per-epoch code-bias step; whether they
are the time-linked code-bias datum's per-key re-seed (RS-3 restored the CBP map with the latch false — did those keys
re-seed?), the W6 set-mean realization, or the receiver side.  Deliverable: mechanism, the smallest increment, and
its seam-epoch bar (ZWD at +1 within 3× the seed's own step; consumer code re-references at the seam ≤ 5).

