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

## 5.08 R0 + 1(d) LANDED AND VALIDATED — the carry re-reference closes the churn

**Built, golden-gated and A/B-validated 2026-08-14, same session as §5.07.** Exe pair: pre-change
`7a908e23…` (the §5.07 snapshot) vs the post-change build; arms `runs/pareref_{off,on}` from
`config/test_suite/_simnet24_inj_pareref_{off,on}.json` (the `_simnet24_inj_null_pdp` posture, one
knob differing). Unit suite 1408/1408 (1 known fixture-gated skip).

### 5.08.1 R0 — all five items, plus two collaterals

1. **PBGAUGE rows are UNCONDITIONAL** (the event sink is always passed; the ledger stays
   armed-only) — §5.07.4 defect 5 closed.
2. **Run-end counters for the three within-epoch responses**
   (`we_refloat_sat/arc/exclude_sta_total`) + a `T2 within-epoch responses` teardown line;
   previously only the withhold had a first-class counter.
3. **σ̂₀_first is traced on every assessed epoch** — 4 columns appended to
   `network_epoch_status.csv` (schema-gated on `raim_within_epoch`, the adm/prires precedent):
   `sigma0_first, n_we_refloat_sats, we_clk_move_max_m, we_clk_move_max_sat`. The null sample R4
   needs now exists on disk.
4. **`n_we_refloat_sats`** carries the tier's OWN whole-sat refloat count (`es.n_refloat_sats` is
   left as the clock-COUPLE set for schema compatibility, with the distinction documented at the
   write site).
5. **The §5.07.2 impulse-channel meter**: the primary solve's clocks are captured after the first
   assess and `(final − first)` is emitted per moved satellite (`we_clock_move` T2 rows) with
   epoch/run maxima. On the OFF arm it reads **28.487 m (G12 @ 305880)** — the tier's own measure
   of the impulse it injects, confirming §5.07.2's decomposition with no arm needed. Collaterals:
   `handover_refused_var` was written-never-read (now on the teardown handover line); the 1(d)
   counters get their own dead-wiring warn.

**Solve neutrality is PROVEN, not asserted**: same config, pre- vs post-change exe —
`network_solution.csv` and `network_dd_fractional.csv` **bit-identical** (sha256),
`network_events.csv` identical after stripping the 235 new R0 rows, `network_epoch_status.csv`
identical after stripping the 4 appended columns.

### 5.08.2 1(d) — `network.phase_anchor_carry_reref` (default OFF)

Exactly §5.07.5 R1: `networkElectPhaseAnchors` populates a per-epoch `phase_anchor_reref`
(sat,signal) set alongside `handovers_out` (parsed from the registered PB key, both cold and
continuous handovers — the continuous pin reverts at T+1 by construction, §5.07.3, so the carry is
stale either way); the D2/C1 fold marginalizes those PB keys for one epoch on BOTH branches,
exactly as `eq34_reref`; the family's phase-leveling local carries are erased at every station
(`networkApplyPhaseAnchorCarryReref`); the VCE monitor and the prior-residual census mirror the
skip (`mech = "phase_anchor_reref"` Frame label). Loader INERT-guards mirror the handover knob's
ladder; NOT in the warm-resume fingerprint (sixth forward-only exemption). Counted at run end;
armed-and-silent warns (invariant #4).

### 5.08.3 The single-knob A/B — every instrument, one direction

Handover schedule **identical 30/30** across the arms (control); gauge audit identical (1
legitimate satellite-left removal, 0 avoidable — control). The 1(d) wiring: 30/30 handovers
marginalized, 171 local carries erased.

| channel | OFF (= §5.0 baseline) | ON 1(d) | factor |
|---|---|---|---|
| CLK_STEP marks / episodes | 15 / 7 | **1 / 1** | — |
| Σ\|dev\| | 160.457 m | **1.529 m** | **105×** |
| datum-free C contrast rms | 18.6553 m | **0.5035 m** | **37×** |
| terminal `clock_std_m` | 15.283 | **0.812** | 18.8× |
| B sat-code-bias truth rms | 1.5725 m | **0.0752 m** | 21× |
| B variance factor | 5526 | **34.7** | 159× |
| rx DCB contrast rms | 4.4141 m | **1.2083 m** | 3.7× |
| T3 cell rate | 6.69 % | **3.66 %** | −45 % |
| T3 consensus rate | 20.1 % | **0.56 %** | 36× |
| within-epoch sat refloats | 8 | **0** | — |
| we clock impulse (max) | 28.487 m | **0** | — |
| σ̂₀_first (terminal) | 2.16 | 1.39 | — |
| consumer 3D final / settled median | 1.649 / 1.450 m | 0.491 / 0.541 m | (§4.1 caveat) |

The one surviving mark is **G12 @ 302430 (1.53 m) — the second epoch of the run, an ignition
transient present in both arms**, not a handover. Every §5.0.1 episode is gone. The ON arm's C
contrast (0.5035) lands AT the §4 clean-election bound (0.518, datum S06 which loses nothing) —
consistent with the §5.07 claim that the entire churn was the un-transformed carry, and achieved
WITHOUT electing a special datum. The tier had nothing left to refloat (8 → 0) and the A4 ceiling
never armed a withhold in either arm.

### 5.08.4 What this changes upstream

- **§5.05's third root is CLOSED on this fixture.** The four null interventions stand as measured
  — they were all design-matrix-side; the first carry-side intervention removes the channel.
- **QUEUE item 4's two blockers are both cleared**: σ̂₀_first now exists on every assessed epoch,
  and `runs/pareref_on` is a VERIFIED null-calibration fixture — at the nine assessed handover
  epochs σ̂₀_first reads 1.37 / 9.35 / 3.80 / 5.88 / 6.99 / 2.30 / 3.29 / 13.27 on the OFF arm
  (the carry tension, twice past the 6.0 A4 ceiling that `sigma0_adapt` laundered — §5.07.4 defect
  1 visible in its own instrument) versus **1.00–1.32 on the ON arm**, i.e. indistinguishable from
  background. The design-side re-pin still happens at those epochs; it is now statistically silent.
- **Item 0 (pin bridge) and item 2 (datum election)** now address exposure to an edge whose carry
  side is fixed — re-rank accordingly (QUEUE amended).

### 5.08.5 What is still NOT established

- **n = 1** — same fixture, arc, constellation and roster salt as everything above. No
  independent-seed replication; no ≥2 h arm; GPS-only.
- **Marginalize (delete) is now MEASURED sufficient here; transform > delete remains unmeasured**
  — the S-transform would additionally preserve one epoch of carry information per handed-over
  family (30 family-epochs here). With the channel at 1.5 m residual Σ|dev| the headroom is small;
  build the transform only if a bigger fixture shows the one-epoch refloat cost. (It is specified
  in **§8**, QUEUE item 1(b), for the CLOCK-PIVOT edge.)
- **Field reachability untested** — unchanged from §5.07.6. Note the flagship field lineages run
  eq34, whose chain election owns `eq34_reref` already; 1(d) covers the prioritized-anchor posture.
- ~~The continuous-handover + 1(d) pairing is legal but unmeasured.~~ **RESOLVED by review
  (§5.08.6): the pairing is loader-DISARMED** — under the ledger pin the frame step lands at T+1
  (the §5.07.3 revert) while the mark covers T, so the pair would marginalize the valid fold and
  miss the stale one. The reref wins the conflict (it alone closed the churn; the continuous
  handover measured null on the same channel), so `phase_datum_handover_continuous` is disarmed
  with a warn whenever both are set. Compose them only after an R2-style sticky target or a
  T+1-pending mark exists AND is validated.

### 5.08.6 Adversarial review of the diff — two confirmed holes, both closed

A 4-dimension / 2-skeptics-per-finding review of the landed diff (7 findings, 4 survived
refutation) confirmed two real edges of the NEW mechanism — both on postures the validation arms
did not exercise — and two minors. All fixed in the same landing:

1. **The continuous pairing mistimed the mark** (major, confirmed twice independently) — see the
   strike-through above. Fixed with a fourth INERT-guard; the validated arms are untouched (they
   ran cold handovers, where mark-at-T is provably correct).
2. **A bridge-exit re-pin was never marked** (major). The bridged-key skip in the election
   `continue`s before any incumbent bookkeeping, so one bridged epoch evaporates the incumbent
   record and the eventual bridge-exit re-pin — a full frame step against a still-stashed carry —
   read as a *first sighting* (standing invariant #7's named failure, verbatim). Fixed by marking
   every **incumbent-less pin**: a family with no surviving carry makes every consumer a no-op,
   while one WITH a surviving carry (bridge exit, or a long-gap return inside the prune horizon)
   is exactly the stale-frame case. First pins at run start mark harmlessly (prev_carry is empty).
3. **A withheld epoch evaporated the mark while the incumbent swap persisted** (minor). The set
   was cleared at the next election, but a withheld epoch skips the stash — its prev_carry still
   holds retired-frame values that would then fold unmarked. Fixed by moving the clear to the
   carry-stash tail: the mark now survives exactly until a solve commits the new frame (SH-gate
   epochs stash and clear correctly; A4/rank-gate epochs persist the mark).
4. **The eq34-only posture disarmed with a factually wrong diagnostic** (minor) — the guard ladder
   now tests eq34 first.

Refuted classes, for the record: the signed-counter vs absolute-column `we_clk_move` split
(documented contract, not a collision), and the guard-ladder-escape scenario (independently
blocked by the `NetworkKnobs` belt-and-braces resolution).

**Post-fix re-verification (same session):** suite 1412/1412; the OFF arm is byte-identical
through the review fixes on all four graded CSVs (the golden chain holds transitively), and the ON
arm's `network_solution.csv` is **bit-identical** to the pre-fix ON arm — every incumbent-less
mark on this fixture was the predicted no-op (126 marks = 30 handovers + 96 first sightings with
no surviving carry), so every number in §5.08.3 stands unchanged for the shipped semantics.

### 5.1 The real fix, not yet built

Both remedies above are avoidance; the defect itself is §1's one-edge event. The correct fix keeps
the network's shared phase-bias set from depending on any single station's visibility at all — the
set follows *network* coverage, the pivot supplies only the gauge pin, and a pivot that loses a
satellite hands that satellite's pin to another station rather than deleting the column. That is
close to what `phase_datum_prioritized` attempts; its measured harm here (§4) says the election
needs the anti-churn treatment (sticky anchors, hysteresis) before it is usable, not that the idea
is wrong.

---

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

## 8. The handover S-transform — QUEUE item 1(b), SPEC'd 2026-08-04, **UNBUILT / OPEN**

*Absorbed 2026-08-15 from `docs/SPEC_handover_stransform.md` (now `docs/attic/`). **Status:
OPEN.** Verified against the tree at absorption: no `handover_stransform`,
`handover_stransform_scope`, `handover_stransform_pb_shadow` or `handover_var_bump` symbol exists
anywhere in `src/` or `include/`. It is live as item **1(b)** of
`docs/QUEUE_principled_replacements.md` and the merge does not retire it. (`docs/README.md` lists
the absorbed card's status as CLOSED — that is wrong; fix at the next index regeneration.)
Memory context of the absorbed card, preserved: `project_network_ssystem_dia_assessment`
(**catalog item 3 + ranked plan 3 — this card**), `project_laser_theory_mapping`,
`project_network_falsified_approaches`, `project_fast_clock_card` (emitted clock ~98 % churn),
`project_agu26_abstract_campaign` (churn, not accuracy, is the blocker). Section mapping for old
citations: the absorbed card's §7 gate list is §8.7 here — its **§7.3** is §8.7 gate 3 (the census
`|t|` cross-check) and its **§7.5** is §8.7 gate 5 / the closing non-goal ("not a consumer fix and
must not be graded as one").*

> ### ⚠ §8 and §5.07/§5.08 are DIFFERENT GAUGES — do not blur them
> **§8 (item 1(b)) TRANSFORMS the CLOCK-PIVOT carry**: the carried priors that are today
> *skipped* for one epoch at five `handover_affects` sites when the per-constellation clock pivot
> changes. **§5.07/§5.08 (item 1(d), `phase_anchor_carry_reref`, LANDED) MARGINALIZES the
> PB-ANCHOR carry**: the phase-bias datum re-realized on the design matrix while the carry stayed
> in the retired frame. Same template — *the carry at a datum edge* — different gauge, different
> detection site, different knob. **1(d)'s validation does NOT close 1(b).**

### 8.1 The transform, and its rank-1 specialization to a pin swap

**Origin.** NGS LASER Guide v2.7 slides 5-86..5-88 (PDF pp. 398–400,
`\\depot\cce_projects\gcap\Software\LASER\`): any minimally-constrained solution transforms to any
other **exactly** by `S := I_m − Eᵀ(KEᵀ)⁻¹K`, applied to mean **AND** dispersion, with only an
l×l inverse (l = the datum defect). Psychas/Khodabandeh (2024) supplies the PPP-RTK
datum-continuity framing.

The clock datum per constellation c is a Dirichlet pin-to-**ZERO** on the pivot satellite
(`pin_y = −piv_x`; note the design-doc drift warning in the DIA catalog — the tree pins to zero,
not to the float value). A handover p→q within c swaps the pin row, and the LASER transform
reduces to a rank-1 update along the gauge null direction:

```
c₀   = x_q_pre / v_c[q]        the new pivot's pre-transform carried level
x   ←  x − v_c · c₀            every carried mean with a component in v_c
var ←  var + v_c[i]² · var_q   diagonal-compatible dispersion update
```

Two load-bearing rules:

1. **`v_c` MUST be the STRUCTURAL null direction `NetworkSbasis` owns** — never hand-written and
   never an eigenvector (SBC lesson: same-scale eigenvectors mix; structural membership is exact
   and carries per-sat identity). After the update the new pivot's carried level is exactly 0 = its
   pinned value, and every other carried mean is what a fresh solve under the new pin would have
   produced.
2. **Dropping the cross term is CONSERVATIVE.** The exact per-column variance of `x_i − v_i·c₀` is
   `var_i + v_i²·var_q − 2v_i·cov(i,q)`. For the dominant common-mode case clocks are positively
   correlated ⇒ `cov(i,q) > 0` ⇒ omitting `−2v_i·cov` **over**-states variance — the same
   "conservative-if-omitted" property that was load-bearing in the ensemble fix. Full `S·D·Sᵀ` on
   the carry blob is deferred (§8.8).

### 8.2 Why — the clock-side handover free-fall is the discontinuity generator

Today a clock-pivot handover is answered by **skipping the fold of every affected carried prior for
one epoch**. `handover_affects` is mirrored at **five** sites:
`src/core/app/NetworkRunContext.cpp:1375`, `NetworkRunContext.cpp:1582`,
`src/core/filter/NetworkClockQc.cpp:92`, `src/core/filter/NetworkSolveGlue.cpp:380`,
`src/core/filter/NetworkStashStatus.cpp:85`. `prev_carry` is re-stashed every epoch
(`NetworkRunContext.cpp:2051`), so the skip is not a persistent loss — it is **one epoch of free
fall**: stiff states (PB at q ≈ 1e-12, CB, clock levels) solve data-only exactly once, re-level,
and re-stash in the new datum. **That one-epoch re-level IS the product discontinuity.**

Motivating quantification: **91 % of emitted PB discontinuities are ref-path re-referencing**
(CLAUDE.md AGU-26 amendment 2) and the emitted clock is **~98 % churn** (FC-1). The information in
the carried prior was **valid** — merely expressed in the old datum, and LASER 5-86 says the
re-expression is exact and cheap. Same discipline SBC's continuity RHS (`vᵀx_prev`,
`NetworkSbasis::completeSbasisDimension`) already applies to deficient directions; there: hold a
direction at its carried level, here: re-express carried levels when the realized datum moves.

**What this card is NOT (FALSIFIED — do not re-litigate):** a consumer fix via datum choice at
emit. Ensemble PB datum, clock s2z and broadcast-anchored were **all A/B'd dead**
(`project_network_falsified_approaches`) — common-mode is consumer-absorbable. §8 is about the
PRODUCER-INTERNAL carry surviving the edge; the wire still steps by the datum delta **by design**
and the existing disc channels still describe it.

### 8.3 Edge classes

| class | detection (exists today) | v1 action |
|---|---|---|
| **A `adjacent`** | `datum_changed` = `pivotHandoverSignalled` (`NetworkSolveGlue.cpp:373`) + `changed_consts`; arm derived at `NetworkSolPrep.cpp:591-614` | **transform** |
| **B `vanish_return`** | same site — `prev_pivot` lacks the key; old pivot from the **persistent** `last_pivot` map (V1/V2 landed, serialized at `NetworkStateSnapshot.cpp:131`/`:303`) | **transform from the last-known realization**; win bounded by surviving-prior count (counter: priors past max-gap aged out ⇒ a counted no-op) |
| **C PB-anchor handover** | prioritized-anchor election change | **SHADOW ONLY** (INC-2) — compute and event the would-be coefficient, apply nothing. That gauge direction spans **PB and ambiguity columns**: bigger surface, measure first |

### 8.4 Design — INC-0..3

**INC-0: the kernel + seam.** New seam `src/core/filter/NetworkHandoverStransform.cpp` (+ manual
`CMakeLists.txt` entry; header `include/filter/NetworkHandoverStransform.hpp`), free function over
`(NetworkRunContext& ctx, const GnssConfig&, …)` per the seam contract, called from the
`NetworkSolveGlue` site where `datum_changed` is derived. It mutates the carried priors in
`ctx.carry` (shared `prev_carry` values; any pl/local key the structural direction touches —
membership is data-driven from `v_c`, not assumed) **BEFORE** the epoch's folds.

**INC-1: release the skips — the load-bearing change.** On an APPLIED transform the five
`handover_affects` skip predicates must **NOT** fire for the transformed keys: the prior is now
valid in the new datum and must fold. Gate each lambda on the applied set; count
`sts_rows_retained` (rows folded that the baseline would have skipped). **Invariant #4: this
counter is the card's ON-path telemetry — a run with transforms applied and
`sts_rows_retained = 0` means the release is mis-wired no matter how clean the log looks.**

**INC-2: both edges are events (invariant #7).** Extend the existing PIVOT event row
(`NetworkSolPrep.cpp:596-613`) with `applied|shadow|skipped` and the coefficient magnitude `|c₀|`;
per-sat identity for every transformed key above a trace floor. Counters
`sts_applied_{adjacent,vanish}`, `sts_shadow_pb`, `sts_rows_retained`, `sts_noop_vanish`, reported
by `NetworkTeardown`. **Cross-epoch reference holders must be audited at build time**: any monitor
comparing values across the edge (the `NetworkClockQc` jump test — it already iterates `prev_pivot`
at `:489`; CB disc jump baselines; relevel baselines) must either receive the same transform or
classify the edge as **FRAME**. The prior-residual census's FRAME ladder is the audit instrument —
after this card a transformed handover edge should show `|t|` **collapse toward CLEAN** on the
affected families, the cleanest single number the card owes (gate 3).

**INC-3: exactness self-check** (cheap, always on when armed). After the transform the new pivot's
carried level must be **exactly 0** and re-deriving the coefficient must return **c₀ = 0**
(idempotence). Assert-with-event, never throw.

### 8.5 Config — four knobs, all default-OFF

| knob | type | default | class / note |
|---|---|---|---|
| `network.handover_stransform` | bool | `false` | master arm; **FINGERPRINTED** — solve-affecting (it changes which priors fold), fresh convergence only, **never flip mid-chain** (the `cb_excursion_reinit` conditional-fingerprint precedent) |
| `network.handover_stransform_scope` | string | `"clock"` | v1 accepts only `"clock"` (classes A+B); loader **ERROR** on anything else |
| `network.handover_stransform_pb_shadow` | bool | `false` | class C measure-only shadow |
| `network.handover_var_bump` | string | `"auto"` | `"auto"` = `v_i²·var_q` per column; or a fixed m² floor added to transformed columns |

Loader site: `loadNetworkCarrySection_`, with the standard requires/mutex validation. **Never legal
on a warm resume whose seed lacked it.**

### 8.6 Cost

Rank-1 over the carried key set, once per handover **EDGE** — not per epoch. Handover rate on the
golden lineage is ~10⁻²/epoch·constellation. Budget: unmeasurable (**< 0.1 ms/edge**); **above
1 ms/edge means the implementation is wrong**.

### 8.7 Field validation — known-answer gates

Controls: an FT63 leg with known handovers + the golden 3 h / 305 lineage. Release only, smoke
first, **seamless lineages** (a warm-resume seam erases the very events under study).

1. **OFF byte-identity** (structural + same-build A/B).
2. **Live wiring**: `sts_applied > 0` **AND** `sts_rows_retained > 0` (invariant #4).
3. **The census cross-check** *(the absorbed card's §7.3)*: on transformed edges the FRAME-label
   `|t|` ladder for `cb`/`pb` collapses toward the CLEAN ladder.
4. **Product churn**: PB discontinuity rate and per-sat clock re-level count across handover edges,
   ON vs OFF. The 91 % ref-path-slip population is the target; expect a measurable cut in the
   **handover-attributed slice specifically** (attribute via the PIVOT events — do not claim the
   slip-attributed slice).
5. **Grade datum-invariant quantities only** (invariant #5): total_IF, DD steps, a00 band. The raw
   wire steps by the datum delta BY DESIGN — a raw-wire comparison is a confound, not a grade.
6. **⚠ THE FALSIFIER WATCH.** The ensemble archive's **epoch-2 signature** — de-medianed states
   matching at the edge epoch then diverging geometrically (~7×/epoch) — means the covariance path
   is wrong (the bump under-states a cross term somewhere). **Halt and report; do NOT tune past it.**

### 8.8 Deferral triage (invariant #3) and non-goals

| deferred | field-blocking? | evidence |
|---|---|---|
| Full `S·D·Sᵀ` on the carry blob | **INERT for v1** — the diagonal bump is conservative for the common-mode-dominated case (§8.1) and gate 6 watches the failure signature | conservatism argument + gate 6; the full form is blocked on a full-cov carry representation (`project_network_fullcov_carry`) |
| Class C apply (PB anchor) | **INERT** — shadow measures the coefficient population first; applying blind to a direction spanning ambiguity columns is the invariant-#1 shape | shadow counters are the successor card's design input |
| Consumer-side transform | **INERT** | producer validation first |
| Event-driven per-const disc + vanish-return persistent map | already landed (V1/V2) — this card CONSUMES them | tree-verified at `NetworkSolPrep.cpp:591-614` |

**What §8 explicitly does NOT do:** it does not change the emitted datum, the wire, or the
disc-channel contract; it does not touch pivot election, SBC, PA or the S-basis gate; it does not
transform covariance beyond the diagonal bump in v1; it does not apply to PB-anchor handovers in
v1 (shadow only); and **it is not a consumer fix and must not be graded as one** *(the absorbed
card's §7.5 = gate 5)*.

**Standing-invariant pre-flight.** (1) The transform's whole purpose is invariant #1 — the stiff
carried prior gets a correct event response (re-expression) instead of a silent free-fall re-level.
(2) Release-only. (3) Deferrals triaged above. (4) `sts_*` counters distinguish "armed, no
handovers" from "never ran". (5) Datum-invariant grading only. (6) Default-OFF, byte-identical,
same-build A/B. (7) Both edges evented with identities (INC-2); the census FRAME ladder measures
what the re-coupling edge moved.

### 8.9 Relationship to 1(d) — what 1(b) still owes

1(d) **marginalizes** the PB-anchor carry and is measured sufficient there (§5.08.3); §8
**transforms** the clock-pivot carry instead of deleting it, on an edge 1(d) never touches.
**"Transform > delete" remains UNMEASURED** (§5.07.6 / §5.08.5) — build the transform when a
bigger fixture shows the one-epoch refloat cost, or for §8's own clock-pivot edge.

## §10 QUEUE item 2 EXECUTED (2026-08-21) — the election is formalized as plumbing; first standard-gate readings

The §4-measured remedy (clean election: 5.9× datum-free contrast, 40 % T3
cut) is now enforced by tooling, not luck.  No producer code changed — every
producer-side alternative in this space is falsified (§ "What this card is
NOT") and stays that way.

- **Generator election** (`scripts/simnet_make_producer_config.py`): with no
  `--datum-station`, the datum is ELECTED — fewest mid-arc losses over the
  post-stride roster (`datum_gauge_audit.compute_rank`), most equatorial
  among ties (§9.8's 8× enforced, not advised), name for determinism (the
  elected value is warm-resume-fingerprinted).  An explicit station absent
  from the roster is a hard error (exit 2, ranking printed) — the old silent
  fall-back to the most-polar `stations[0]` is gone.  `--constellations`
  matches the producer's active systems (a satellite the producer does not
  estimate cannot cost a gauge event).
- **Field-config preflight** (`datum_gauge_audit.py --config <json>`):
  flattens the extends chain (`gnss_config_chain.load_config`) and gates the
  declaration — set (exit 1 on the `stations.front()` default), in-roster,
  not polar (≥55°) while an equatorial (≤30°) alternative exists
  (`--allow-polar` downgrades to a warning), marking channel armed
  (`emit_clock_disc` + `clock_disc_jump_test`), datum-station slip response
  armed (invariant #1).  Continuity is documented as NOT statically
  checkable — `--run` is the post-hoc, truth-free gate (exit 1 on any
  avoidable removal, both modes sharing the harness-gate exit).
- **Validation against this SPEC's own record**: the 24-sta GPS-only
  election returns **S06000SIM (0 losses)** — arm B, reproduced; the 60-sta
  GPS+GAL election returns **S30000SIM (10 losses, −0.9°)**, winning the
  equatorial tiebreak over S26000SIM (10 losses, −8.1°) — the camp60
  campaign's hand-elected datum, retro-validated by the formalized
  criterion.  All fail paths verified (unset ⇒ exit 1; absent ⇒ exit 2, no
  config written).
- **First standard-gate readings**: `--run runs/camp60_i3` (elected datum)
  ⇒ **0 avoidable removals, exit 0**.  `--run runs/fld63_relevel4h_on`
  (field, MBAR00UGA) ⇒ **12 avoidable pivot-only removals over 4 h, ALL
  unmarked** — exit 1.  The preflight predicts the silence exactly: the
  ext63 lineage arms `emit_clock_disc` but NOT `clock_disc_jump_test`, so
  every avoidable re-realization is silent to the consumer; it also runs
  without `datum_station_detect`.  MBAR00UGA's geometry is fine (−0.6°);
  its §4.2 "arrived at by luck" continuity did not survive a 4 h window.
  Filed for the ext63 lineage owners: arm the marking channel
  (fingerprint-SAFE trace-class per the armed-set rules) and re-read; the
  12 events are the datum-election exposure item 1(b)'s S-transform would
  close producer-internally.

## §11 Item 1(b) precondition PRICED (2026-08-21) — the refloat cost measures near-zero; the build stays deferred with a concrete trigger

§8.9's standing verdict ("build the transform when a bigger fixture shows the
one-epoch refloat cost") was written when the only fixture had ONE roster
event in 1 h.  The bigger fixtures now exist, and the cost is measured from
data on disk:

| lineage | window | sticky_pivot | PIVOT/handover edges | amputation |
|---|---|---|---|---|
| fld63_relevel4h (63-sta field, GPS dual) | 4 h | **true** | **0** | zero |
| camp60_i3 / _f4 (60-sta sim, GPS+GAL, slipped) | 6 h | off | **3** (G×2, E×1, all `adjacent`) | 3 single-epoch fold-skips per run vs 720 epochs |

The §5.08.5 "30 family-epochs" headroom figure belongs to the PB-anchor edge
on the pdp posture — not this edge.  On every current lineage the clock-pivot
handover flux is either zero (sticky election, RTA-1, doing exactly its job)
or ~1 edge / 2 h with a one-epoch cost the prires census prices as noise.
The mechanism's target population is measured near-empty — the
`emit_pb_filter_gate_v2` lesson ("its advertised win has an empty target
population here"), applied BEFORE the build this time.

**Disposition: 1(b) remains deferred-conditional, precondition now PRICED
rather than assumed.**  The §8 design (rank-1 transform along the structural
null, INC-0..3, four knobs) is fully derived and stays ready.  Concrete build
triggers, any of: (a) a lineage that must run non-sticky election with
measured handover flux ≳ 1/h; (b) a vanish-return-heavy roster (arm
`pivot_vanish_return_signal` and read the PIVOT arms mix first); (c) a
measured per-edge consumer cost at an `adjacent` handover (the A2-marked
epoch class) that the one-epoch free-fall demonstrably owns.  First response
on any lineage showing flux is `network.sticky_pivot`, not the transform.
Census note for the next pricing: the camp60 arms ran
`prior_residual_families: "iono"` only, so FRAME/mech=handover rows were not
censused there — a future pricing arm should widen the families to see the
per-edge amputation directly.

---

## §5.09 R2 EXECUTED (2026-08-31) — the sticky continuous target, and the pairing COMPOSED

**Trigger.** The 12 h zero-slip quiet flagship (`runs/camp60wu_quiet12h`,
SPEC_synthetic_minimum_network §17) put hard numbers on §5.08's residual: with the
reref armed and the continuous handover loader-disarmed, **582 of 621 anchor
handovers took the COLD path and the 1(d) response erased 10 236 phase-leveling
local carries over the run — on a fixture with zero true slips**.  Handover count
itself is near the geometric floor (per-(sat,signal) keys × pass durations;
0 bounce-backs — the sticky election is fine).  The cost is the response: every
~2.6 h per satellite, each family's carries die network-wide and every float arc
restarts, which is exactly the churn × arc-truncation mechanism that keeps the
clock/bias/iono separation code-limited (AGU-26 "churn, not accuracy, blocks").

**The build (this section's diff).**
- `NetworkCarryState::anchor_pin_target` — the pin target each incumbent is
  currently realized at, per "sat_signal" key.  Populated ONLY by continuous
  handovers; rebuilt wholesale each election beside `anchor_incumbent` (a key
  that leaves the union, or hands over cold, drops its target); never
  snapshotted (a warm resume's first handover is honestly cold); empty forever
  on the knob-OFF path, which keeps the OFF election byte-identical.
- `networkElectPhaseAnchors`: a RETAINED incumbent with a stored target keeps
  realizing it — the §5.07.3 T+1 revert (pin back to 0, family steps one epoch
  late) is gone.  Chains compose: the ledger is harvested in whatever frame the
  standing pins realize, so successive continuous handovers extend one frame.
- The 1(d) mark narrows to COLD re-realizations: `(is_handover && !continuous)
  || incumbent-less`.  A continuous handover under the sticky target provably
  realizes the same frame at T and after, so its family's carried state stays
  in-frame — marking it would marginalize a valid fold and erase live carries.
  The incumbent-less clause (first sighting / bridge exit, §5.08.6 finding 2)
  is untouched.
- Loader: the §5.08.6 pairing disarm is REPLACED by the composition (info log).
  The §5.0 measured-null verdict stands for the UNSTICKY mechanism it graded;
  R2 is the "compose only after an R2-style sticky target exists" condition
  that section itself set.

**⚠ The ceiling is now load-bearing for CARRY VALIDITY, not just datum sanity.**
A continuous handover preserves the frame only to the ledger value's own error
ε (≤ √max_var 1σ), and the surviving carries are mm-stiff — an ε of decimetres
would inject visible tension (the unexplained G21 +17.07 m / 378 σ incident in
§5.0 was accepted under the default 1.0 m² ceiling).  The armed configs
therefore run `network.phase_datum_handover_max_var_m2: 1e-4` (σ ≤ 1 cm): a
settled arc (mm-class after hours of zero-q carry) passes; a fresh arc fails to
the honest COLD path, which is the §5.0-validated behavior.  The loader default
stays 1.0 for compatibility; the info line now nags about tightness.

**Tests.** `tests/test_phase_datum_handover.cpp` (29 cases): sticky persistence
across tenure, cold-drop of the stored target, union-exit pruning, the
cold-marks/continuous-skips split in one election, and the loader composition
(`CarryRerefComposesWithTheContinuousPairing` replaces the disarm case).

**Validation ladder** (this session): OFF-path byte-identity old-exe vs new-exe
on a 120-ep smoke of the 12 h fixture; ON smoke counter sanity (continuous ≫
cold, erasures ≈ first sightings); then the 12 h flagship re-run graded against
truth (SPEC_synthetic_minimum_network §17) — results recorded below when run.

### §5.09.1 First ON arm FALSIFIED the ceiling's QUANTITY — border-exact re-validation (same session)

The first 12 h R2 arm produced 582 handovers with only **30 continuous — all 552
cold were REFUSED ledger values** (no misses), uniformly across the day, not just
in ignition.  The refusals were not young arcs: settled DD ambiguity σ implies
per-column relative variance ~1.2e-05 m², an order under the 1e-4 ceiling.  The
harvest was measuring the WRONG VARIANCE: `conditionalLocalVarianceDiag`
propagates the FREE SHARED BORDER's covariance into the local diagonal (the
right flavor for the leveling carry's prior, which re-injects against next
epoch's border), but in the handover step the family's shared phase-bias
uncertainty moves the pin target and the realized frame TOGETHER — common-mode,
cancels in the step.  Gating on the border-inclusive marginal therefore (a)
refused every settled successor whose family bias was momentarily loose, and (b)
retro-explains §5.0's "measured but unexplained" G21: a +17.07 m target passed
the 1.0 m² ceiling while landing a 378 σ spike, because a tight border can mask
a poor RELATIVE value exactly as a loose border masks a good one.

Fix: `NetworkEstimator::pureConditionalLocalVarianceDiag` — diag(N_rr⁻¹) on the
requested columns with the entire border treated as known (Σ_ss ≡ 0) — consumed
by the ledger harvest only; the leveling carry keeps its border-inclusive
flavor.  The ceiling now bounds the successor's error RELATIVE to the shared
frame, which is the quantity the surviving carries actually feel.

### §5.09.2 The 12 h A/B verdict — churn dead, consumer 2×, and the trade named

`runs/camp60wu_quiet12h` (baseline) vs `runs/camp60wu_quiet12h_r2` (single
mechanism: the composed pairing + 1e-4 border-exact ceiling), 1440/1440 both,
same fixture/build/datum.  (`runs/camp60wu_quiet12h_r2v1` is the §5.09.1
falsified-ceiling arm, kept as evidence.)

- **Mechanism: closed.**  582 handovers: 573 CONTINUOUS / 9 cold (all
  refused-var = honest fresh arcs; zero ledger misses).  Reref erasures
  10 236 → 165 (−98.4 %); marked family-epochs 789 → 216 (≈ first sightings).
  Largest avoided frame step 29.6 m.
- **Consumer (the product): the headline.**  3D med by quarter
  0.49/0.59/0.51/0.23 → 1.13/1.23/**0.16/0.19** m — R2 pays a worse ignition
  (frames lock while still raw) then converges and STAYS; settled-half rms
  0.493 → 0.265 m (−46 %), up rms 0.331 → 0.151 m (−54 %).  AR 0/1440 in both
  (pre-registered: this card was never the AR unblock — the per-band integer
  wall is iono-repr + float-only, unchanged).
- **Clock level walk: UNCHANGED** (rms by quarter 0.39/1.35/1.94/1.85 →
  0.27/1.32/1.99/2.01) — the §5.08 residue exactly as recorded: the per-sat
  walk is Q3 estimator work and the level needs an external authority; R2
  neither fixed nor worsened it.
- **The lock-in tax, named:** code-bias settled rms 0.39 → 0.54 m
  (p50 1.4 → 2.5 cm); ZWD converges to the same endpoint slightly slower
  (Q3 7.3 vs 9.3 cm, Q4 3.8 vs 4.3 cm).  The baseline's constant cold
  refloats were, perversely, a crude level re-tap toward code truth; with
  arcs surviving, whatever level the early run established persists.  That
  is the standing LEVEL-AUTHORITY frontier (LV-6), now unmasked rather than
  randomized — not a new defect.
- **vf's double across channels** (clock 15.5k → 33.4k) with σ_med TIGHTER
  (clk 1.5 → 1.1 cm): retention accumulates formal information faster than
  physical error shrinks — the carry-aware-covariance σ-honesty frontier,
  more exposed, as that card predicts for any retention gain.

**Disposition.**  The R2 pairing is VALIDATED on the quiet sim at 60 sta / 12 h
for what it claimed: frame + carry survival and product-stream continuity, with
the consumer as the beneficiary.  It stays default-OFF, armed scoped in
`_camp60wu_quiet12h_r2.json`; a field arm inherits the usual fresh-convergence
rule.  Follow-ups it sharpens rather than closes: the cb/level lock-in wants
the LV-6 per-epoch level authority; the vf inflation wants the carry-aware σ;
the clock walk stays Q3.
