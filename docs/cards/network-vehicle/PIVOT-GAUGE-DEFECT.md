# pivot gauge defect: the single-pivot phase-bias gauge defect and its third root

**STATUS**

- **id:** pivot gauge defect
- **topic:** network-vehicle
- **status:** closed
- **summary:** PROVEN with truth: a datum-station satellite loss re-realizes the phase-bias gauge into the satellite clock; clean election is the cheap remedy, `phase_datum_prioritized` NET-HARMFUL here, and a third, carry-side root named.
- **dates in the text:** 2026-08-12 → 2026-08-15
- **knobs / posture:** `network.phase_anchor_carry_reref`, `network.phase_datum_prioritized`, `network.emit_clock_disc`, `network.clock_disc_jump_test`, `network.phase_datum_handover_continuous`, `network.phase_datum_handover_max_var_m2`, `network.phase_datum_time_linked`, `network.raim_within_epoch`
- **hashes named in the text** (commit / exe / byte-cert): `7a908e23` (the §5.07 A/B exe snapshot)
- **run dirs named in the text:** `runs/simnet60_tri`, `runs/pc305_ggq_tri_slant_4h`, `runs/pc305_ggq_tri_slant_1h`, `runs/simnet300_tri_slant`, `runs/inj_weab_{on,off}`, `runs/inj_null`, `runs/inj_null_pdp`, `runs/simnet60_tri_slant_480_ddint`, and five more in the text
- **source:** `docs/archive/SPEC_pivot_visibility_gauge_defect.md` (preamble), §1 to §7 (§5.08 and §5.1 are card R0 + 1(d), §5.09 is card R2 sticky, §8 is card queue 1(b))

**Outcome.** The question: what drives a coherent 12 m clock excursion on a zero-fault fixture. When the datum station alone loses a satellite, that satellite's phase-bias columns leave network-wide and its clock absorbs the step: predicted 3/3 at 24 stations and 6/6 at 60, live and unmarked on the field. A clean datum gives a 5.9× better datum-free clock contrast. The continuous handover was built and does not close it (each episode fires one epoch later). §5.07 names the root: the re-realization reaches the design matrix, never the carry. The carry fix is [R0 + 1(d)](../network-vehicle/PIVOT-CARRY-REREF-R0-1D.md); [RF-2](../../refound/RF-2_held_basis_2026-09-15.md) now declares every handover.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_pivot_visibility_gauge_defect.md -->
# SPEC — the single-pivot phase-bias gauge defect

**Also carries, since 2026-08-15, the handover S-transform card as §8** (QUEUE item **1(b)**,
absorbed from `docs/SPEC_handover_stransform.md`, **UNBUILT / OPEN**). §8 is the *clock-pivot*
carry edge; §5.07/§5.08 is the *PB-anchor* carry edge — same template, different gauges.

**Status ladder** (newest first):

- **2026-08-14 (later).** §5.07's root fix is BUILT and VALIDATED — R0 telemetry landed
  (solve-neutral, golden gate PASS) and QUEUE item 1(d) (`network.phase_anchor_carry_reref`,
  default OFF) closes the clock churn on the 24-station fixture: **Σ|dev| 160.46 → 1.53 m
  (105×), episodes 7 → 1 (the ignition mark), datum-free C contrast 18.66 → 0.50 m (37×), at
  the clean-election bound**. → **§5.08**.
- **2026-08-14.** §5.05's THIRD root NAMED and A/B-confirmed — the phase-bias datum re-realizes
  on the design matrix only, the carry is never transformed into the new frame, and the
  satellite clock absorbs the step. → **§5.07**, which also strikes §5.06's "not a gauge problem
  at all" conclusion and annotates §5.05's second falsified hypothesis as a no-exposure test.
- **2026-08-13.** Root cause PROVEN with truth on the synthetic fixture, causally confirmed by a
  three-arm prediction test at 24 stations and reproduced 6/6 at 60 stations — the sim
  reproduction of a defect the codebase already knew in the field ("G29 −44 m step, JPLM
  masked"). Cheap remedy: DATUM ELECTION. The shipped remedy `network.phase_datum_prioritized`
  is measured NET-HARMFUL on this fixture. Audit tool: `scripts/datum_gauge_audit.py`.

Found while chasing an anomaly in the injection bench's null arm
(`SPEC_synthetic_minimum_network.md` §15.6 — the absorbed `SPEC_sim_injection_bench.md` §5.5,
now `docs/attic/`): a coherent, network-wide residual excursion on a fixture with zero injected
faults.

---

## 1. The defect

With `network.phase_datum_prioritized` OFF — **the default, and what every arm in this lineage
runs** — the network's shared phase-bias set is taken wholesale from one station:

```cpp
if (!phase_datum_prioritized) {
    shared_phase_keys.clear();
    for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
        shared_phase_keys.insert(kv.first);
}                                          // ApplicationNetwork.cpp:1627-1630
```

and `stations.front()` **is the clock datum station** — `clock_datum_station` becomes
`roster.reference`, which occupies index 0 (`ApplicationNetworkSetup.cpp:181-186`). So when the
pivot **alone** stops seeing a satellite, that satellite's phase-bias columns are removed
**network-wide**, even though every other station is still tracking it, and the released
direction has nowhere to go: the satellite **clock absorbs it**. This is standing invariant #7
with an unbounded decoupling edge — nothing bounds what the clock may absorb, and the
re-coupling edge (each station's ambiguity refloating) is unmanaged and arrives several epochs
later, staggered.

The code's own comment on the *other* branch names the failure exactly — prioritized election
picks "the highest-coverage receiver that tracks it with a live ambiguity as its anchor —
decoupled from the clock pivot, **so a satellite low/unseen at the clock pivot keeps a clean
gauge at another station instead of jumping**" — as does the producer's POSTURE line:
`POSTURE OFF [SCOPED] network.phase_datum_prioritized` — "cures the single-pivot gauge
re-reference (G29 −44 m step, JPLM masked)".

## 2. The observed signature

24-station fixture, 120 epochs, field-N regime, **zero injected faults**, datum `S12000SIM`. At
tow 303510 the pivot loses G15 — and in that one epoch:

```
CLK_STEP , mark        , G15 , -12.055485 , thr 1.500000 , retro_bump
TOPOLOGY , col_removed , label=PB:G15:L1C   (also L2W, L5Q)
```

Every station still tracking G15 sees its post-fit phase residual jump from ~1–3 to
**400–2214** (standardized, ≈ 6.6 m). The RAIM attribution tier then refloats them, **two
stations per epoch over three epochs** — S20/S23 at 303510, S15/S18 at 303540, S22/S24 at
303570 — each station's residual dropping to exactly 0.00 the epoch after its own refloat.

**The clock does not recover.** `worst_dev_m` for G15 is 11.88 m at the event and still 11.30 m
150 s later; the network's `clock_std_m` doubles (1.00 → 2.32) and stays there for the rest of
the run. A consumer applying these clocks sees G15 step 12 m. **The sim truth is clean through
all of it** — phase windup smooth to ±0.0005 cyc/epoch with no discontinuity, G15 continuously
visible at every affected station, no scripted or random slips. The excursion is entirely
producer-side.

## 3. Causal proof — a two-sided prediction, 3/3 then 6/6

The mechanism predicts excursions **exactly** at the epochs where *the datum station* loses a
satellite the rest of the network still tracks. Tested by changing only `clock_datum_station`:

| datum | predicted | PB col_removed | CLK_STEP | peak cell |
|---|---|---|---|---|
| S12000SIM (loses G15 @303510) | G15@303510 | **G15 ×3 @303510** ✔ | **−12.06 m** ✔ | 2214.5 |
| S06000SIM (loses nothing) | **none** | **none** ✔ | **none** ✔ | **36.1** |
| S13000SIM (loses G07 @304290) | G07@304290 | **G07 ×3 @304290** ✔ | **−13.19 m** ✔ | 1882.3 |

Reproduced on the independent **60-station** fixture (`runs/simnet60_tri`, datum `S26000SIM`):
**6 of 6** PB-only removals matched a datum-station satellite loss to the exact epoch.

### 3.1 The discriminator — and it needs no truth

The 60-station arm also showed removals that were *not* pivot losses, and they separate
perfectly: **PB only** (CLK retained) = satellite still in the network, only the pivot lost it =
**AVOIDABLE defect**; **PB + CLK + CB** = satellite left the network entirely = legitimate.
9 events at 60 stations, 6 PB-only / 3 PB+CLK+CB, 100 % clean separation. Because this reads
only `network_events.csv` it works on **field runs**, where no truth exists — that is what
`scripts/datum_gauge_audit.py --run` implements.

## 4. Remedies, measured

Four arms, identical fixture and config, one knob each:

| arm | peak cell | T3 rate | **C-contrast rms** (datum-free) | B rms | rx DCB rms |
|---|---|---|---|---|---|
| A · datum S12 (loses G15) | 2214.5 | 7.47 % | 3.073 m | 0.166 | 1.309 |
| **B · datum S06 (loses none)** | **36.1** | **4.49 %** | **0.518 m** | **0.078** | 1.215 |
| C · datum S13 (loses G07) | 1882.3 | 4.67 % | 3.210 m | 0.069 | 1.216 |
| D · datum S12 + `phase_datum_prioritized` | 118.0 | 6.69 % | **18.655 m** | 1.573 | 4.414 |

**Datum election is the cheap, large win.** Electing a pivot that never loses a satellite gives a
**5.9× better datum-free satellite-clock contrast** (3.07 → 0.52 m), 2.1× better code bias, a
40 % cut in T3 false alarms, and a **61× lower peak residual** — at zero cost, with no knob.

**`phase_datum_prioritized` is NET-HARMFUL here.** It does suppress the specific excursion
(2214 → 118), but the datum-free clock contrast degrades **6×** against the bad-datum baseline
and **36×** against clean election, code bias degrades 9×, and `CLK_STEP` marks go **1 → 15** —
one large re-realization traded for fifteen smaller ones, consistent with per-epoch churn in its
anchor election. Its SCOPED promotion-ledger entry is not contradicted (it *does* cure the
re-reference it claims to), but on this fixture the cure costs more than the disease.

### 4.1 ⚠ Three statistics that will mislead you here

- **`clock_std_m` / `worst_dev_m` run medians are confounded by event TIMING.** Arm A's event
  falls at epoch 38 of 120 (82 damaged epochs ⇒ damaged median); arm C's at epoch 64 (56 damaged
  ⇒ clean median). The medians read 2.17/10.96 for A and 0.94/2.28 for C, inverting the truth
  that both arms suffered an equivalent event. Use the datum-free contrast, which ranks them
  equal (3.073 vs 3.210) as it should.
- **Raw wire clock spread is not a verdict** (invariant #5). Arm D changes the phase-bias gauge,
  so its larger raw spread is partly frame realization. The C-record *contrast* is the
  datum-free channel and is what the table reports.
- **The consumer 3D rms does not separate these arms** (0.838 / 0.796 / 0.464 / 1.058 m) at 60
  settled epochs — arm C, with a bad datum, reads best. Do not quote the consumer as evidence
  for datum election on an arc this short.

## 4.2 FIELD EVIDENCE — the defect is live on the current lineage, and SILENT

`scripts/datum_gauge_audit.py --run` needs only `network_events.csv`, so it runs on archived
field output. Applied to the DOY-129 305-station arms:

| run | PB removals | pivot-only (avoidable) | satellite-left (legit) |
|---|---|---|---|
| `runs/pc305_ggq_tri_slant_4h` | 7 | **2** (G01 @262500, @262590) | 5 |
| `runs/pc305_ggq_tri_slant_1h` | 3 | **2** | 1 |
| `runs/simnet300_tri_slant` | 8 | **8** | 0 |

The field count is low because `_pc305_ggq_tri_slant_4h.json` elects **MBAR00UGA** — an
equatorial station that loses almost nothing — which is §5.1's remedy arrived at by luck rather
than by criterion. The 300-station sim arm, whose datum came from the `--datum-station` default,
carries **8 of 8** avoidable.

**Both field events are UNMARKED.** That lineage resolves `network.emit_clock_disc` and
`network.clock_disc_jump_test` to *unset*, so no `CLK_STEP` record exists for either — the clock
re-realization is of unknown magnitude and **completely silent to the consumer**. This is the
concrete instance of the standing warning that without `emit_clock_disc` the per-satellite
clock-disc counter is never populated and product-wide jumps go unreported. The audit tool flags
exactly this case rather than reporting "no step".

## 5. What to do

1. **Elect the datum station on satellite-visibility continuity.**
   `scripts/datum_gauge_audit.py --fixture <dir> --constellations G` ranks the roster by mid-arc
   losses. *Necessary, not sufficient*: §9.8 of `SPEC_synthetic_minimum_network.md` measured 8×
   more common-level wander from a polar pivot, so rank by losses first and prefer an equatorial
   station among the ties.
2. **Audit existing runs, including field runs**, with `scripts/datum_gauge_audit.py --run <dir>`
   — exit code 1 when any avoidable removal is present, so a harness can gate on it. It also
   reports pivot-only removals carrying **no** `CLK_STEP` mark — re-realizations *silent to the
   consumer*, the condition `emit_clock_disc` exists to prevent.
3. **Do NOT reach for `phase_datum_prioritized` as the fix** without re-measuring the datum-free
   contrast on the target lineage.

## 5.0 THE TWO-EDGE HANDOVER — BUILT, and it does NOT close the defect

`network.phase_datum_handover_continuous` (default OFF), landed with
`network.phase_datum_handover_max_var_m2` (default 1.0 m²).

**What it does.** On an anchor handover the election pins the INCOMING anchor at its own absolute
ambiguity estimate **in the outgoing frame** — from a one-epoch ledger harvested post-solve off the
carry-neutral `qc_amb` census — instead of re-zeroing it, so the realization is unchanged by
construction. Both edges are counted (`handover_continuous` / `handover_cold` /
`handover_refused_var`) and emitted as `PBGAUGE` trace rows (outgoing anchor, incoming anchor,
target applied). The re-coupling edge re-validates before reconnecting: a ledger value whose
conditional variance exceeds the ceiling is refused to the honest COLD path, because the pin is a
hard ~1e10-weight constraint and an ill-determined estimate would be injected *as a datum*.

**⚠ Why `x_lin` cannot be used.** `getStateVector()(amb_idx)` at election time is a LINEARIZATION
FRAME, not an estimate — the network solve is never written back into the per-station StateManager,
and an `x_lin` shift on a local column is a proven exact solve no-op. The absolute estimate is
`x_lin + back-substituted Δx`, which exists only after the solve; hence the one-epoch deferral, the
same shape as `carry_innov_pending` / `dw_release_pending` / `raim_refloat_pending`.

**Verification: 14 unit cases; OFF path byte-identical on a same-build golden comparison**
(`network_{events,solution,dd_fractional,epoch_status}.csv` bit-identical, all four graded channels
equal to 4 decimals). Mechanically it works — 27 of 30 handovers made continuous on the 24-station
arm, largest avoided step **17.07 m**. **And it does not deliver** (single-knob A/B, same build,
24-station fixture):

| | peak | p99 | CLK_STEP | T3 | **C-contrast rms** | B rms | consumer |
|---|---|---|---|---|---|---|---|
| OFF | 118.0 | 37.7 | 15 | 6.69 % | 18.655 m | 1.573 | 1.058 |
| ON | 378.4 | 35.9 | 16 | 6.60 % | **18.604 m** | 1.480 | 0.644 |

The datum-free clock contrast moves 0.3 %; clock steps go 15 → 16. One key (G21, the largest target
at +17.07 m) shows a *higher* peak in the ON arm and the 1.0 m² variance ceiling did not refuse it —
measured but unexplained. The consumer and B improvements are not claimed (§4.1: the consumer
channel does not separate arms at this arc length). **Landed default-OFF as a correct primitive,
NOT as a validated remedy** — it is the invariant-#7-compliant way to move a gauge and will be
needed by whatever does close this, but on the evidence it is not itself the fix.

### 5.0.1 Re-graded on EPISODES — and the fix defers by exactly one epoch

`CLK_STEP` mark count is a poor discriminator: 9 of the 16 marks are one satellite (G15) crossing
the 1.5 m threshold on nine consecutive epochs. The marking is nonetheless *correct* —
`NetworkClockEmitRebase.cpp:84-100` computes `dev` against the **previous epoch's emitted value**
and advances `prev_[sat].last_held_m` unconditionally every epoch, so each mark is a genuinely NEW
common-removed step, not one event re-marked against a stale baseline (checked and falsified).
Re-grading on **episodes** — contiguous mark-runs per satellite:

| | marks | **episodes** | Σ\|dev\| | max \|dev\| |
|---|---|---|---|---|
| OFF | 15 | **7** | 160.5 m | 36.51 |
| ON | 16 | **7** | 157.1 m | 36.52 |

| sat | OFF onset | ON onset | Σ\|dev\| OFF → ON |
|---|---|---|---|
| G21 | 303420 | **303450** | 22.5 → 20.0 |
| G23 | 303420 | **303450** | 18.8 → 19.1 |
| G15 | 303510 | **303540** | 41.3 → 40.1 (9 epochs both) |
| G25 | 303660 | **303690** | 18.8 → 18.8 |
| G07 | 304290 | **304320** | 21.1 → 21.1 |
| G12 | 305880 | **305910** | 36.5 → 36.5 |

**Every episode fires exactly one epoch (30 s) later with the fix armed, at near-identical
magnitude** — the signature of a ledger that is ONE EPOCH STALE: the value pinned at *t* is the
successor's estimate in the frame of *t−1*, and by *t* that frame has moved. Sharper than "no
effect": the mechanism acts on the right quantity and is defeated by its own deferral. Closing it
needs a same-epoch estimate of the successor's ambiguity, which does not exist before the solve —
either a two-pass epoch (solve, retarget, re-solve) or a predicted value with its own uncertainty,
both larger cards than this one. The 2 % cumulative improvement and the uniform one-epoch shift are
also consistent with a simpler reading: these seven episodes are threshold crossings of a quantity
the handover does not control, and arming the knob perturbs the solve just enough to move each
crossing by one epoch. §5.05's three falsified hypotheses stand either way.

## 5.05 What the A/B ruled OUT — the two defects are different

**Under the prioritized union the legacy defect is already gone.** The whole 120-epoch prioritized
run contains exactly **one** phase-bias column removal (G20, `PB+CLK+CB` — a satellite genuinely
leaving the network), against six pivot-only removals on the legacy arm. So the proven −12.06 m
defect of §1–§3 is closed by electing the union, not by anything in §5.0.

**The prioritized arm's own 15–16 clock steps are a THIRD, still-open root.** Three hypotheses
tested and falsified:

| hypothesis | test | result |
|---|---|---|
| they are anchor handovers | compensate all 30 (§5.0) | **no effect** on C-contrast or step count |
| they are column-set changes | correlate CLK_STEP with `col_removed` on the same sat | **0 / 15** ⚠ *no exposure — see below* |
| the CLOCK datum station's visibility does it, as the PB datum's did | correlate first CLK_STEP per sat with that station's satellite losses | **1 / 6** |

G15 alone carries 9 of the 16 steps as a decaying tail (+19.2, +4.2, +3.7, +3.3, +2.8, +1.9,
+1.6, +1.7, +1.6 m over consecutive epochs) — a slow re-settle, not a single re-realization.

⚠ **Hypothesis 2 was a test with no exposure (noted 2026-08-14).** It was measured *inside the
prioritized arm*, which by this section's own count contains **exactly one** column removal (G20);
zero correlation against a population of one is not a falsification. The test with exposure is the
**cross-posture** one in §5.07: pdp-OFF's two PB column deaths ARE pdp-ON's two pivot-outgoing
handovers, same epochs, same satellites — and the post-warmup one carries a 12.055 m clock step.
Hypotheses 1 and 3 stand as measured.

## 5.06 The PIN BRIDGE already does what a two-pass retarget would — measured

A true two-pass epoch (solve → retarget the pin → re-solve) is structurally expensive:
`NetworkEstimator::addStation` throws once `solved_` is set (`NetworkEstimator.cpp:192-194`), so
changing a pin target requires `beginEpoch()` plus a **full PASS-B re-assembly** — ~2× the
per-epoch cost on every handover epoch, not an RHS back-substitution. It is also largely
unnecessary. `NetworkAnchorHygiene.cpp:231`'s
`want_bridge = bridge_on && !reopen_barred && prior_fresh && (min_arc_ep <= 0 || !eligible_exists)`
is reached only when `handover_pending`, and `min_arc_ep <= 0` is the DEFAULT — so with
`network.phase_datum_time_linked` armed, **every handover is bridged**: the key keeps its shared
phase bias at `pb_level_prev` through a soft row (σ = 0.05 m) with **no station pin at all**. The
gauge moves to the SHARED side, station-independently — the outcome a retarget targets, in one pass,
reachable from the prioritized anchor election today (not eq34-only). Measured on the same
24-station arm with `phase_datum_time_linked` as the single knob — **27 bridges opened, 453
row-folds, `0 reattaches performed`** (the gauge never moved to another station at all):

| arm | marks | episodes | Σ\|dev\| | peak | T3 | **C-contrast** | **B rms** | consumer |
|---|---|---|---|---|---|---|---|---|
| pdp baseline | 15 | 7 | 160.5 | 118.0 | 6.69 % | 18.655 | 1.573 | 1.058 |
| + handover_continuous | 16 | 7 | 157.1 | 378.4 | 6.60 % | 18.604 | 1.480 | 0.644 |
| **+ pin BRIDGE** | 15 | **8** | 165.3 | 131.8 | 7.55 % | **17.214** | **0.813** | 0.845 |

**The bridge is a real win on the code-bias channel — B rms 1.573 → 0.813 m, 1.9×** — and 7.7 %
on the datum-free clock contrast. It does NOT reduce the clock re-realizations: 7 → 8 episodes,
Σ|dev| 160.5 → 165.3 m, T3 slightly worse. **That is the FOURTH mechanism to leave the clock
episodes untouched** (datum election on the prioritized path, handover-continuous, the pin bridge,
and the coverage gate).

> ⚠ **STRUCK 2026-08-14.** This section previously concluded from those four nulls that *"the
> clock churn is not a phase-bias gauge problem at all."* **That inference was wrong.** It IS a
> gauge problem; all four probes were on the *design-matrix* side of an edge whose defect is on
> the *carry* side. See §5.07.

A two-pass epoch would differ from the bridge in one respect only: its target would be the CURRENT
epoch's solved value rather than the previous epoch's level, and it would be a hard pin rather
than a soft row. Given four null results on the clock channel its expected value there is low; it
is not scheduled.

## 5.07 THE THIRD ROOT, NAMED — the datum re-realizes on the design matrix only

**Status: named 2026-08-14, confirmed by a decisive single-knob A/B (`runs/inj_weab_{on,off}`,
same exe snapshot `7a908e23…`).**

> **A satellite's phase-bias datum is realized by a hard pin (w ≈ 1e10) on one station-arc. When
> that arc dies at the elevation mask the datum re-realizes — as a cold re-pin at `target = 0.0`
> (`NetworkPhaseDatum.cpp:339`) under `phase_datum_prioritized`, or as an outright column drop
> under the pivot-hosted posture — and the re-realization is applied to the DESIGN MATRIX ONLY.
> The carried state is never transformed into the new frame. Because `sat_phase_bias_q = 1e-12`
> freezes bˢ_σ while `sat_clock_q = 0.01` leaves δtˢ loose — a 1e10:1 ratio — the entire frame
> step lands on the satellite CLOCK.**

The clock is not malfunctioning. It is the **designated absorber** of a gauge change nobody
transformed.

### 5.07.1 The cross-posture identity

`runs/inj_null` (pdp OFF) contains exactly two phase-bias column deaths in the whole run;
`runs/inj_null_pdp` (pdp ON) contains ten anchor handovers, of which exactly two have the pivot
`S12000SIM` as the *outgoing* anchor. **They are the same two epochs and satellites:**

| tow | `inj_null` (pdp OFF) | `inj_null_pdp` (pdp ON) |
|---|---|---|
| 302490 | `col_removed PB:G12:{L1C,L2W,L5Q}` | `anchor_handover G12 S12000SIM→S04000SIM` |
| 303510 | `col_removed PB:G15:{L1C,L2W,L5Q}` | `anchor_handover G15 S12000SIM→S23000SIM` |

The **event** is the host arc dying at the mask — all ten baseline handovers are preceded at t−1
by the outgoing anchor's last sighting of that satellite at 10.01–10.20°, never re-acquired. The
posture only chooses the *delivery form*. Both put a metric step on the clock (−12.055 m OFF,
+19.482 m ON at 303510), and pdp multiplies exposure from **2 host-deaths to 10** — the whole
Σ|dev| 12.06 → 160.46 difference.

**The absorber is the clock, not the bias.** G21, TOW 303390 → 303420
(`network_solution.csv`): CLK −1.9944 → **−24.4221** (Δ −22.428 m); phase biases move ≤ **0.54 m**
on all three signals; code bias moves **1.1 mm**. Against `q_clock·dt = 0.30 m²` (σ ≈ 0.55 m) that
is a **41 σ** clock innovation.

### 5.07.2 The decisive A/B — the QC tier is the impulse channel, not the source

`network.raim_within_epoch` OFF vs ON, same build, same obs root, handover schedule verified
**byte-identical (10/10, same epoch / sat / from→to)** because the election runs at
`ApplicationNetwork.cpp:1682`, strictly upstream of the solve glue at `:2002`.

| channel | pdp OFF (`inj_null`) | tier ON | tier OFF |
|---|---|---|---|
| `we_refloat_sat` | 0 | 8 | **0** |
| anchor handovers | 0 | 10 | 10 |
| CLK_STEP marks | 1 | 15 | **32** |
| Σ\|dev\| | 12.055 m | 160.457 m | **178.366 m** |
| terminal `clock_std_m` | 2.1478 | 15.2826 | **14.6259** |
| `worst_dev_m` | 10.8274 | 60.0605 | **56.4554** |

**Deleting the entire within-epoch tier does not remove the re-realization.** The competing root
(the whole-sat refloat is the source) predicted `clock_std_m` would collapse toward the pdp-OFF
2.15 m; it measured **14.63**, a 4.3 % change. Same destination, different delivery — G21 clock,
tier ON vs OFF: tow 303390 −1.9944 / −1.9944; **303420 −24.4221 (−22.43 in one epoch) / −5.5308
(−3.54)**; 303450 −23.4492 / **−23.4955**; 303480 −22.4683 / −22.5462 . G23 behaves identically
(−18.79 in one step vs −5.99 then −18.08). The refloat is the **impulse channel**: mean |dev| per mark
**10.697 → 5.574 m**, the same motion redistributed into 32 smaller steps instead of 15 large
ones. Corroboration at scale: `runs/simnet300_tri_slant` has 40 `we_refloat_sat`, 0 handovers,
**0 CLK_STEP**; `runs/simnet60_tri_slant_480_ddint` 38 / 0 / 0.

⚠ **Two honest misses, recorded rather than dropped:** (1) **the mark-count prediction was
WRONG** — the named root predicted CLK_STEP would *fall* as steps became ramps; it **rose,
15 → 32**, because the ramp is still steep enough to keep clearing the 1.5 m/epoch bar. The
mechanism direction (impulse → distributed) is confirmed by the per-epoch decomposition; only the
instrument prediction failed, which independently reinforces §5.07.4's point that the per-epoch
step test is the wrong detector. (2) **The G28 control is INCONCLUSIVE, not passed** — it diverges
up to 1.30 m, and G28's handover is at 304650, long after the arms first diverge at 303420, so it
was never isolated. What it does show: G28 ramps ~12 m in **both** arms with no refloat in either
— independent support from a satellite the tier never touched.

### 5.07.3 Why the four gauge interventions were null — and why the shift is one epoch

All four act on the **design-matrix** side (WHO hosts, WHAT target, WHEN). **None touches the
carry side.** The previous epoch's bˢ_σ — a value in the *retired* frame — is folded into the new
frame at `q_pb = 1e-12` (`NetworkRunContext.cpp:1709-1711`) because `PHASE_ANCHOR` reaches neither
carry-skip guard: `datum_changed` reads the **clock-pivot map only**
(`NetworkSolveGlue.cpp:461-462`), and `eq34_reref` is populated **only** in
`NetworkChainDatum.cpp:778`.

The one-epoch shift of §5.0.1 is then structural, not mysterious. `NetworkPhaseDatum.cpp:339` sets
`double target = 0.0`, overridden only `if (is_handover && handover)`. At T+1 `is_handover` is
false, so **the target reverts to 0.0** — the full cold re-pin, one epoch late. The σ̂₀ trip moves
with it because it is downstream: the handover schedule is byte-identical across the arms while
the refloat *and* the step both move. A downstream effect moves; a cause does not.

### 5.07.4 Collateral defects found while proving this

1. **The A4 backstop is defeated by construction.** `sigma0_adapt` publishes the *post*-adapt σ̂₀
   (`NetworkSolveGlue.cpp:1667`). σ̂₀_first reached **13.27** against a **6.0** withhold ceiling,
   and `n_sigma0_withheld = 0` on every arm — the adaptation launders the epoch past its own gate.
2. **The σ̂₀ pool is blind where the tension lands.** `NetworkSolveGlue.cpp:1206` skips the pivot
   from the pool whenever pdp is off — i.e. excludes the one station carrying the datum. A
   permanent 12.055 m re-realization moves σ̂₀ by 0.055 and T2 stays silent.
3. **`raim_within_epoch: false` is silently overridden.** `GnssConfigLoadNetwork.cpp:3473`
   force-enables the tier whenever `raim_withhold_sigma0 > 0`. This is why all 11 `inj_*` arms
   carry the tier armed — nobody chose it. Disabling it requires zeroing the ceiling too;
   legitimate here only because the ceiling provably never fires (`n_sigma0_withheld = 0`,
   `n_gate_withheld = 0`, 120/120 emitted).
4. **The step test misses cumulative drift.** G28 ramps −0.70 → −11.98 m, permanent, with **zero
   marks** — every increment sits under the 1.5 m/epoch bar. Conversely G21's −22.5 m step
   **recovers 86 %** by run end, so it is not a re-realization at all. The "7 episodes" census is
   contaminated at both ends and needs a cumulative/level companion, not a smaller per-epoch bar.
5. **Dead telemetry (invariant #4).** `es.n_refloat_sats` reads `clock_refloat_sats.size()`
   (`NetworkStashStatus.cpp:512`) ⇒ **0 on all 120 rows** while `n_adapt` is nonzero on 7.
   `PBGAUGE` two-edge rows are emitted **only** under `phase_datum_handover_continuous`
   (`ApplicationNetwork.cpp:1685-1688`), so the cold re-pin is an unlogged both-edge event in the
   default posture.

### 5.07.5 The remedy, ranked

**PRIMARY is BOUND THE EDGE; fixing the trigger's null is required but strictly second** — the
mask crossing happens regardless of any statistic, the pdp-OFF twin carries the defect with 1 trip
and 1 mark, and the largest untouched item (G28's −11.28 m) has neither a trip nor a mark.

- **R0 — telemetry first** (zero solve risk, gates everything below): emit `PBGAUGE`
  unconditionally; run-end counters for the three within-epoch responses; **trace σ̂₀_first on
  every live epoch** (it exists only inside the `detail` span at `:1640`, so *there is no null
  sample on disk to fit*); fix `es.n_refloat_sats`; capture the primary-solve clock at `:1495` and
  emit `(final − first)`. ✅ **LANDED 2026-08-14, all five items — §5.08.1.**
- **R1 — the carry re-reference (primary).** Populate a `phase_anchor_reref` `{sat, signal}` set in
  `networkElectPhaseAnchors` alongside `handovers_out` (`NetworkPhaseDatum.cpp:377-379`) and
  consume it at `NetworkRunContext.cpp:1706-1707` **exactly as `eq34_reref` is** — the template is
  already built and already commented for this failure at `:1701-1705`. Erase `local_carry` for
  **all** stations of that (sat,signal). Default-OFF, byte-identical when off. ✅ **LANDED AND
  VALIDATED 2026-08-14 as `network.phase_anchor_carry_reref` — §5.08.2/§5.08.3.**
- **R2 — make the continuous target sticky past the handover epoch**, or
  `phase_datum_handover_continuous` remains a one-epoch deferral by construction. *(Moot under
  1(d)-alone — with the carry marginalized, the cold T and T+1 pins realize the SAME frame, so
  nothing steps at the revert. R2 matters only for the continuous+1(d) pairing, which is
  unmeasured.)*
- **R3 — declare what still moves**: a cumulative companion to the step test. Still open.
- **R4 — fix the null** (second, not optional): derive the actual null of
  `max(floor, 1.4826·median(·))` over correlated standardized residuals; give the statistic a
  prior-side term (`SPEC_prior_residual_census.md` §14.2); extend `networkClockAnchorGuard`'s
  pattern (`NetworkSolveGlue.cpp:1074-1077`) to a phase-datum pre-adaptation guard.
- **R5 — do NOT do first: `clock_monitor_couple_raim`.** Double-gated
  (`NetworkRunContext.cpp:96`), and **85.5 % of Σ|dev| is emitted before the stash it acts
  through** — its reach is 2.7 %.

### 5.07.6 What is NOT established

- **Marginalize vs transform.** R1 follows the `eq34_reref` template, which *deletes* the prior
  for one epoch. The theoretically correct action is an **S-transform** of the (sat,signal) carry
  family. That transform > delete is INFERRED, unmeasured.
- **Transient vs permanent re-realizations are unexplained.** G21's step recovers 86 % (identical
  shape in `inj_bridge`); G15's does not.
- **Field reachability UNTESTED.** The T2 tier is `carry_warm`-gated and no field run on disk
  carries a `T2_within_epoch` event — but **the root is not gated on `phase_leveling`** (the
  pdp-OFF twin exhibits it with 0 T2 events), and `_day129_guards_conv.json` arms both. No field
  events CSV was inspected.
- **n = 1.** 24 stations, 120 epochs, one hour, GPS-only, a single roster salt.
- **A second bare edge inside the same handover is unmeasured**: the outgoing anchor becomes
  carry-eligible again with a `local_carry` entry harvested *before* it anchored; the incoming
  anchor's carry is silently dropped. Same predicate (`networkIsGaugePinned_`) both sides, no
  event on either.

## 6. Relationship to the T3 false-alarm anomaly

This defect contributes to, but does not explain, the T3 post-fit RAIM implied-α anomaly
(`SPEC_synthetic_minimum_network.md` §15.6; the absorbed `SPEC_sim_injection_bench.md` §5.5).
Electing a clean datum cuts the T3 flag rate 7.47 % → 4.49 %, so roughly **40 %** of the false
alarms in the worst arm are downstream of an avoidable gauge event. A **4.5 % floor remains**, and
that floor is the separate finding: the T3 statistic is a per-cell **max over rows** normalized by
a **median over individual rows**, then compared against a Gaussian 6σ quantile. The
implied-scale-versus-quantile curve rises monotonically (1.83 → 3.18 → 4.49 → 6.14 → 313) with
excess kurtosis 4391, so the residue is shape, not scale.

✅ **The floor is now DERIVED — `docs/SPEC_t3_null_derivation.md` (2026-08-14), fitting on this
spec's own §5.08 null instrument `runs/pareref_on`.** Three amendments:

- **"Threshold/population mismatch" was the right words for the wrong object.** The statistic is a
  ratio of two functionals of the SAME pool and is therefore **invariant to any common rescaling
  of z** — so the mismatch cannot be in the threshold at all. Under the exact per-cell null (max
  over n_c = 3 rows) the shipped k = 6 is an α = 5.92e-9 test, i.e. **already stricter than
  1e-6**, and the `max(1,·)` floor on σ̂₀ makes it conservative on top. The max-over-rows
  correction the item-4 plan called for is **3.00×** against an observed **1.86e7×**.
- **The population half is exactly right, and now measured.** On a fixture with ONE global noise
  model the settled pool carries a **4.7× station** and **2.75× signal** scale spread (epoch:
  1.3×). Out-of-sample station stratification takes the flag rate **4.46 % → 0.97 %**.
- ⚠ **Excess kurtosis 4391 does not reproduce** on the post-1(d) arm — settled `s_raw` reads
  **10.38**. Cite the derivation card, not this figure.

The 1(d) fix is upstream of a real share of this channel: the T3 cell rate moved 7.468e-02
(`runs/inj_null`) → 6.692e-02 (prioritized) → **3.663e-02** (1(d)).

---

## 7. Reproduction

```bash
# the three prediction arms (24 sta, 120 ep, ~29 s each, Release only)
python scripts/datum_gauge_audit.py --fixture runs/simnet24_inject_null --constellations G
#   -> S06000SIM has 0 losses; S12000SIM loses G15; S13000SIM loses G07
#   edit clock_datum_station in config/test_suite/_simnet24_inj_ds_<STA>.json, then
build/x64-Release/gnss_processor.exe config/test_suite/_simnet24_inj_ds_S06000SIM.json

# audit any run, field included — needs only network_events.csv
python scripts/datum_gauge_audit.py --run runs/inj_null_l13 runs/inj_ds_S06000SIM

# the datum-free grade that ranks the arms honestly
python scripts/simnet_grade.py runs/simnet24_inject_null --run <run> --products <products.txt>
#   read the C-record CONTRAST line, not clock_std_m

# --- §5.07: the decisive within-epoch A/B (2 x ~35 s, Release only) ---
# ⚠ Run BOTH arms on ONE exe snapshot — the arms on disk predate a rebuild, and
#   invariant #6 forbids cross-build comparison.
cp build/x64-Release/gnss_processor.exe build/x64-Release/gnss_processor_weab.exe
sha256sum build/x64-Release/gnss_processor{,_weab}.exe   # must match
build/x64-Release/gnss_processor_weab.exe config/test_suite/_simnet24_inj_weab_on.json
build/x64-Release/gnss_processor_weab.exe config/test_suite/_simnet24_inj_weab_off.json
#   logs land in ./logs (CWD = repo root); copy to runs/inj_weab_<arm>/logs between runs.
# ⚠ raim_within_epoch:false ALONE IS A NO-OP — GnssConfigLoadNetwork.cpp:3473
#   force-enables the tier whenever raim_withhold_sigma0 > 0.  The OFF config zeroes
#   the ceiling too; legal ONLY because the ON twin measures
#   n_sigma0_withheld = 0 / n_gate_withheld = 0 / 120 emitted (ceiling never fires).
for d in inj_weab_on inj_weab_off; do
  awk -F, '$4=="anchor_handover"{print $2,$5,$10}' runs/$d/logs/network_events.csv | sort -u
done   # -> must be 10/10 identical, else the A/B has a second variable loose

# --- §5.08: the 1(d) validation pair (2 x ~35 s, Release only, ONE exe snapshot) ---
build/x64-Release/gnss_processor.exe config/test_suite/_simnet24_inj_pareref_off.json
build/x64-Release/gnss_processor.exe config/test_suite/_simnet24_inj_pareref_on.json
python scripts/clk_step_episodes.py runs/pareref_off/logs runs/pareref_on/logs
#   -> OFF: 15 marks / 7 episodes / 160.457 m;  ON: 1 / 1 / 1.529 m (the ignition mark)
python scripts/simnet_grade.py runs/simnet24_inject_null --run runs/pareref_on \
    --products runs/pareref_on/products.txt      # C contrast 0.5035 m
# control: the anchor_handover schedules must be identical 30/30 across the arms
```

Configs: `_simnet24_inj_null_l13.json` (datum S12, `trace_station_residuals` on),
`_simnet24_inj_ds_S06000SIM.json`, `_simnet24_inj_ds_S13000SIM.json`,
`_simnet24_inj_null_pdp.json` (`phase_datum_prioritized` ON), `_simnet24_inj_weab_{on,off}.json`
(§5.07), `_simnet24_inj_pareref_{off,on}.json` (§5.08).

> **Run-path provenance (one note for the whole file).** `runs/` was archived off `C:` on
> 2026-08-12 to `D:\GnssProcessor_archive\2026-08-12\runs\`. Every `runs/<name>` cited above that
> predates that date — the `pc305_ggq_*` field arms, `simnet60_tri`, `simnet300_tri_slant`,
> `simnet60_tri_slant_480_ddint` — resolves under the archive, not the working tree. The `inj_*`
> and `pareref_*` arms (2026-08-13/14) postdate the archive and are in the working tree.

---

