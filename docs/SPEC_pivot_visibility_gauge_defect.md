# SPEC — the single-pivot phase-bias gauge defect

**Status, 2026-08-13.** Root cause PROVEN with truth on the synthetic fixture,
causally confirmed by a three-arm prediction test at 24 stations and reproduced
6/6 at 60 stations. This is the sim reproduction of a defect the codebase already
knew about in the field ("G29 −44 m step, JPLM masked"). The cheap remedy is
DATUM ELECTION; the shipped remedy `network.phase_datum_prioritized` is measured
NET-HARMFUL on this fixture. Audit tool: `scripts/datum_gauge_audit.py`.

Found while chasing an anomaly in the injection bench's null arm
(`SPEC_sim_injection_bench.md` §5.5): a coherent, network-wide residual excursion
on a fixture with zero injected faults.

---

## 1. The defect

With `network.phase_datum_prioritized` OFF — **the default, and what every arm in
this lineage runs** — the network's shared phase-bias set is taken wholesale from
one station:

```cpp
if (!phase_datum_prioritized) {
    shared_phase_keys.clear();
    for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
        shared_phase_keys.insert(kv.first);
}                                          // ApplicationNetwork.cpp:1627-1630
```

and `stations.front()` **is the clock datum station** — `clock_datum_station`
becomes `roster.reference`, which occupies index 0
(`ApplicationNetworkSetup.cpp:181-186`).

So when the pivot **alone** stops seeing a satellite, that satellite's phase-bias
columns are removed **network-wide**, even though every other station is still
tracking it. The released direction has nowhere to go and the satellite **clock
absorbs it**.

This is standing invariant #7 with an unbounded decoupling edge. Nothing bounds
what the clock may absorb when the column vanishes, and the re-coupling edge —
each station's ambiguity refloating — is unmanaged and arrives several epochs
later, staggered.

The code's own comment on the *other* branch names the failure exactly:

> *"Prioritized: elect, per (sat,signal), the highest-coverage receiver that
> tracks it with a live ambiguity as its anchor — decoupled from the clock pivot,
> **so a satellite low/unseen at the clock pivot keeps a clean gauge at another
> station instead of jumping.**"*

as does the producer's own POSTURE line:

> `POSTURE OFF [SCOPED] network.phase_datum_prioritized` — *"legacy leg when eq34
> is off: cures the single-pivot gauge re-reference (G29 −44 m step, JPLM masked)"*

---

## 2. The observed signature

24-station fixture, 120 epochs, field-N regime, **zero injected faults**, datum
`S12000SIM`. At tow 303510 the pivot loses G15 — and in that one epoch:

```
CLK_STEP , mark        , G15 , -12.055485 , thr 1.500000 , retro_bump
TOPOLOGY , col_removed , label=PB:G15:L1C
TOPOLOGY , col_removed , label=PB:G15:L2W
TOPOLOGY , col_removed , label=PB:G15:L5Q
```

Every station still tracking G15 sees its post-fit phase residual jump from ~1–3
to **400–2214** (standardized units; ≈ 6.6 m). Then the RAIM attribution tier
refloats them, **two stations per epoch over three epochs** — S20/S23 at 303510,
S15/S18 at 303540, S22/S24 at 303570 — and each station's residual drops to
exactly 0.00 the epoch after its own refloat.

**The clock does not recover.** `worst_dev_m` for G15 is 11.88 m at the event and
still 11.30 m 150 s later; the network's `clock_std_m` doubles (1.00 → 2.32) and
stays there for the rest of the run. A consumer applying these clocks sees G15
step 12 m.

**The sim truth is clean through all of it** — phase windup is smooth to
±0.0005 cyc/epoch with no discontinuity, G15 is continuously visible at every
affected station, and the fixture contains no scripted or random slips. The
excursion is entirely producer-side.

---

## 3. Causal proof — a two-sided prediction, 3/3 then 6/6

The mechanism predicts excursions occur **exactly** at the epochs where *the
datum station* loses a satellite the rest of the network still tracks. Tested by
changing only `clock_datum_station`:

| datum | predicted | PB col_removed | CLK_STEP | peak cell |
|---|---|---|---|---|
| S12000SIM (loses G15 @303510) | G15@303510 | **G15 ×3 @303510** ✔ | **−12.06 m** ✔ | 2214.5 |
| S06000SIM (loses nothing) | **none** | **none** ✔ | **none** ✔ | **36.1** |
| S13000SIM (loses G07 @304290) | G07@304290 | **G07 ×3 @304290** ✔ | **−13.19 m** ✔ | 1882.3 |

Reproduced on the independent **60-station** fixture (`runs/simnet60_tri`, datum
`S26000SIM`): **6 of 6** PB-only removals matched a datum-station satellite loss
to the exact epoch.

### 3.1 The discriminator — and it needs no truth

The 60-station arm also showed removals that were *not* pivot losses, and they
separate perfectly:

| columns removed | meaning | verdict |
|---|---|---|
| **PB only** (CLK retained) | satellite still in the network, only the pivot lost it | **AVOIDABLE defect** |
| **PB + CLK + CB** | satellite left the network entirely | legitimate |

9 events at 60 stations, 6 PB-only / 3 PB+CLK+CB, 100 % clean separation. Because
this reads only `network_events.csv`, it works on **field runs**, where no truth
exists — that is what `scripts/datum_gauge_audit.py --run` implements.

---

## 4. Remedies, measured

Four arms, identical fixture and config, one knob each:

| arm | peak cell | T3 rate | **C-contrast rms** (datum-free) | B rms | rx DCB rms |
|---|---|---|---|---|---|
| A · datum S12 (loses G15) | 2214.5 | 7.47 % | 3.073 m | 0.166 | 1.309 |
| **B · datum S06 (loses none)** | **36.1** | **4.49 %** | **0.518 m** | **0.078** | 1.215 |
| C · datum S13 (loses G07) | 1882.3 | 4.67 % | 3.210 m | 0.069 | 1.216 |
| D · datum S12 + `phase_datum_prioritized` | 118.0 | 6.69 % | **18.655 m** | 1.573 | 4.414 |

**Datum election is the cheap, large win.** Electing a pivot that never loses a
satellite gives a **5.9× better datum-free satellite-clock contrast** (3.07 →
0.52 m), 2.1× better code bias, a 40 % cut in T3 false alarms, and a **61× lower
peak residual** — at zero cost, with no knob.

**`phase_datum_prioritized` is NET-HARMFUL here.** It does suppress the specific
excursion (2214 → 118), but the datum-free clock contrast degrades **6×** against
the bad-datum baseline and **36×** against clean election, code bias degrades 9×,
and `CLK_STEP` marks go **1 → 15**. It trades one large re-realization for fifteen
smaller ones, consistent with per-epoch churn in its anchor election. Its SCOPED
promotion-ledger entry is not contradicted — it *does* cure the re-reference it
claims to — but on this fixture the cure costs more than the disease.

### 4.1 ⚠ Two statistics that will mislead you here

- **`clock_std_m` / `worst_dev_m` run medians are confounded by event TIMING.**
  Arm A's event falls at epoch 38 of 120 (82 damaged epochs ⇒ damaged median);
  arm C's at epoch 64 (56 damaged epochs ⇒ clean median). The medians read
  2.17/10.96 for A and 0.94/2.28 for C, which inverts the truth that both arms
  suffered an equivalent event. Use the datum-free contrast, which ranks them
  equal (3.073 vs 3.210) as it should.
- **Raw wire clock spread is not a verdict** (invariant #5). Arm D changes the
  phase-bias gauge, so its larger raw spread is partly frame realization. The
  C-record *contrast* is the datum-free channel and is what the table above
  reports.
- The **consumer 3D rms does not separate these arms** (0.838 / 0.796 / 0.464 /
  1.058 m) at 60 settled epochs — arm C, with a bad datum, reads best. Do not
  quote the consumer as evidence for datum election on an arc this short.

---

## 4.2 FIELD EVIDENCE — the defect is live on the current lineage, and SILENT

`scripts/datum_gauge_audit.py --run` needs only `network_events.csv`, so it runs
on archived field output. Applied to the DOY-129 305-station arms:

| run | PB removals | pivot-only (avoidable) | satellite-left (legit) |
|---|---|---|---|
| `runs/pc305_ggq_tri_slant_4h` | 7 | **2** (G01 @262500, @262590) | 5 |
| `runs/pc305_ggq_tri_slant_1h` | 3 | **2** | 1 |
| `runs/simnet300_tri_slant` | 8 | **8** | 0 |

The field count is low because `_pc305_ggq_tri_slant_4h.json` elects
**MBAR00UGA** — an equatorial station that loses almost nothing — which is the
remedy of §5.1 arrived at by luck rather than by criterion. The 300-station sim
arm, whose datum was chosen by the `--datum-station` default, carries **8 of 8**
avoidable.

**Both field events are UNMARKED.** That lineage resolves
`network.emit_clock_disc` and `network.clock_disc_jump_test` to *unset*, so no
`CLK_STEP` record exists for either — the clock re-realization is of unknown
magnitude and **completely silent to the consumer**. This is the concrete
instance of the standing warning that without `emit_clock_disc` the per-satellite
clock-disc counter is never populated and product-wide jumps go unreported. The
audit tool flags exactly this case rather than reporting "no step".

## 5. What to do

1. **Elect the datum station on satellite-visibility continuity.**
   `scripts/datum_gauge_audit.py --fixture <dir> --constellations G` ranks the
   roster by mid-arc losses. This is a *necessary* criterion, not a sufficient
   one: §9.8 of `SPEC_synthetic_minimum_network.md` measured 8× more common-level
   wander from a polar pivot, so rank by losses first and prefer an equatorial
   station among the ties.
2. **Audit existing runs, including field runs**, with
   `scripts/datum_gauge_audit.py --run <dir>`. Exit code 1 when any avoidable
   removal is present, so a harness can gate on it. It also reports pivot-only
   removals that carry **no** `CLK_STEP` mark — those are re-realizations that are
   *silent to the consumer*, the condition `emit_clock_disc` exists to prevent.
3. **Do NOT reach for `phase_datum_prioritized` as the fix** without re-measuring
   the datum-free contrast on the target lineage.

## 5.0 THE TWO-EDGE HANDOVER — BUILT, and it does NOT close the defect

`network.phase_datum_handover_continuous` (default OFF), landed with
`network.phase_datum_handover_max_var_m2` (default 1.0 m²).

**What it does.** On an anchor handover the election pins the INCOMING anchor at
its own absolute ambiguity estimate **in the outgoing frame** — taken from a
one-epoch ledger harvested post-solve off the carry-neutral `qc_amb` census —
instead of re-zeroing it. The realization is then unchanged by construction.
Both edges are counted (`handover_continuous` / `handover_cold` /
`handover_refused_var`) and emitted as `PBGAUGE` trace rows carrying the outgoing
anchor, the incoming anchor and the target applied. The re-coupling edge
re-validates before it reconnects: a ledger value whose conditional variance
exceeds the ceiling is refused to the honest COLD path, because the pin is a hard
~1e10-weight constraint and stamping in an ill-determined estimate injects that
estimate's error *as a datum*.

**⚠ Why `x_lin` cannot be used.** `getStateVector()(amb_idx)` at election time is
a LINEARIZATION FRAME, not an estimate — the network solve is never written back
into the per-station StateManager, and an `x_lin` shift on a local column is a
proven exact solve no-op. The absolute estimate is `x_lin + back-substituted Δx`,
which exists only after the solve; hence the one-epoch deferral, the same shape as
`carry_innov_pending` / `dw_release_pending` / `raim_refloat_pending`.

**Verification.** 14 unit cases. OFF path byte-identical on a same-build golden
comparison: `network_{events,solution,dd_fractional,epoch_status}.csv` bit-identical
and all four graded channels equal to 4 decimals. Mechanically it works — 27 of 30
handovers made continuous on the 24-station arm, largest avoided step **17.07 m**.

**And it does not deliver.** Single-knob A/B, same build, 24-station fixture:

| | peak | p99 | CLK_STEP | T3 | **C-contrast rms** | B rms | consumer |
|---|---|---|---|---|---|---|---|
| OFF | 118.0 | 37.7 | 15 | 6.69 % | 18.655 m | 1.573 | 1.058 |
| ON | 378.4 | 35.9 | 16 | 6.60 % | **18.604 m** | 1.480 | 0.644 |

The datum-free clock contrast moves 0.3 %. Clock steps go 15 → 16. One key (G21,
the largest target at +17.07 m) shows a *higher* peak in the ON arm, and the
variance ceiling at 1.0 m² did not refuse it — so that regression is measured but
unexplained. The consumer and B improvements are not claimed: §4.1 already showed
the consumer channel does not separate arms at this arc length.

**The mechanism is landed default-OFF as a correct primitive, NOT as a validated
remedy.** It is the invariant-#7-compliant way to move a gauge, and it will be
needed by whatever does close this; but on the evidence it is not itself the fix.

### 5.0.1 Re-graded on EPISODES — and the fix defers by exactly one epoch

`CLK_STEP` mark count is a poor discriminator: 9 of the 16 marks are one
satellite (G15) crossing the 1.5 m threshold on nine consecutive epochs. The
marking is nonetheless *correct* — `NetworkClockEmitRebase.cpp:84-100` computes
`dev` against the **previous epoch's emitted value** and advances
`prev_[sat].last_held_m` unconditionally every epoch, so each mark is a genuinely
NEW common-removed step, not one event re-marked against a stale baseline. (That
hypothesis was checked explicitly and falsified.)

Re-grading on **episodes** — contiguous mark-runs per satellite:

| | marks | **episodes** | Σ\|dev\| | max \|dev\| |
|---|---|---|---|---|
| OFF | 15 | **7** | 160.5 m | 36.51 |
| ON | 16 | **7** | 157.1 m | 36.52 |

Seven episodes either way, cumulative re-realization down 2 %. The fix is confirmed
null on this channel — but the episode listing shows something the aggregate hid:

| sat | OFF onset | ON onset | Σ\|dev\| OFF → ON |
|---|---|---|---|
| G21 | 303420 | **303450** | 22.5 → 20.0 |
| G23 | 303420 | **303450** | 18.8 → 19.1 |
| G15 | 303510 | **303540** | 41.3 → 40.1 (9 epochs both) |
| G25 | 303660 | **303690** | 18.8 → 18.8 |
| G07 | 304290 | **304320** | 21.1 → 21.1 |
| G12 | 305880 | **305910** | 36.5 → 36.5 |

**Every episode fires exactly one epoch (30 s) later with the fix armed, at
near-identical magnitude.** The continuous pin holds the frame for exactly one
epoch and the re-realization then happens anyway — which is precisely the
signature of a ledger that is ONE EPOCH STALE: the value pinned at epoch *t* is
the successor's estimate in the frame of *t−1*, and by *t* that frame has moved.

That is a sharper result than "no effect": the mechanism demonstrably acts on the
right quantity and is defeated by its own deferral. Closing it needs a
same-epoch estimate of the successor's ambiguity, which does not exist before the
solve — so either a two-pass epoch (solve, retarget, re-solve) or a predicted
value with its own uncertainty, both of which are larger cards than this one.

The 2 % cumulative improvement and the uniform one-epoch shift are also consistent
with a simpler reading: these seven episodes are threshold crossings of a quantity
the handover does not control, and arming the knob perturbs the solve just enough
to move each crossing by one epoch. §5.05's three falsified hypotheses stand
either way — the same seven satellites step by the same amounts in both arms.

## 5.05 What the A/B ruled OUT — the two defects are different

Testing the fix produced a sharper diagnosis than the fix itself.

**Under the prioritized union the legacy defect is already gone.** The whole
120-epoch prioritized run contains exactly **one** phase-bias column removal (G20,
`PB+CLK+CB` — a satellite genuinely leaving the network), against six pivot-only
removals on the legacy arm. So the proven −12.06 m defect of §1–§3 is closed by
electing the union, not by anything in §5.0.

**The prioritized arm's own 15–16 clock steps are a THIRD, still-open root.**
Three hypotheses tested and falsified:

| hypothesis | test | result |
|---|---|---|
| they are anchor handovers | compensate all 30 (§5.0) | **no effect** on C-contrast or step count |
| they are column-set changes | correlate CLK_STEP with `col_removed` on the same sat | **0 / 15** |
| the CLOCK datum station's visibility does it, as the PB datum's did | correlate first CLK_STEP per sat with that station's satellite losses | **1 / 6** |

G15 alone carries 9 of the 16 steps as a decaying tail (+19.2, +4.2, +3.7, +3.3,
+2.8, +1.9, +1.6, +1.7, +1.6 m over consecutive epochs) — a slow re-settle, not a
single re-realization. Whatever drives it is not any gauge-move mechanism this
card examined.

## 5.06 The PIN BRIDGE already does what a two-pass retarget would — measured

A true two-pass epoch (solve → retarget the pin → re-solve) is structurally
expensive: `NetworkEstimator::addStation` throws once `solved_` is set
(`NetworkEstimator.cpp:192-194`), so changing a pin target requires `beginEpoch()`
plus a **full PASS-B re-assembly** — ~2× the per-epoch cost on every handover
epoch, not an RHS back-substitution.

It is also largely unnecessary. `NetworkAnchorHygiene.cpp:231`:

```cpp
const bool want_bridge = bridge_on && !reopen_barred && prior_fresh
                         && (min_arc_ep <= 0 || !eligible_exists);
```

reached only when `handover_pending`, and `min_arc_ep <= 0` is the DEFAULT — so
with `network.phase_datum_time_linked` armed, **every handover is bridged**: the
key keeps its shared phase bias at `pb_level_prev` through a soft row
(σ = 0.05 m) with **no station pin at all**. The gauge moves to the SHARED side,
station-independently — which is the outcome a retarget targets, in one pass, and
it is reachable from the prioritized anchor election today (not eq34-only).

Measured on the same 24-station arm — `phase_datum_time_linked` is the single
knob; **27 bridges opened, 453 row-folds, and `0 reattaches performed`** (the
gauge never moved to another station at all):

| arm | marks | episodes | Σ\|dev\| | peak | T3 | **C-contrast** | **B rms** | consumer |
|---|---|---|---|---|---|---|---|---|
| pdp baseline | 15 | 7 | 160.5 | 118.0 | 6.69 % | 18.655 | 1.573 | 1.058 |
| + handover_continuous | 16 | 7 | 157.1 | 378.4 | 6.60 % | 18.604 | 1.480 | 0.644 |
| **+ pin BRIDGE** | 15 | **8** | 165.3 | 131.8 | 7.55 % | **17.214** | **0.813** | 0.845 |

**The bridge is a real win on the code-bias channel — B rms 1.573 → 0.813 m,
1.9×** — and 7.7 % on the datum-free clock contrast. It does NOT reduce the clock
re-realizations: 7 → 8 episodes, Σ\|dev\| 160.5 → 165.3 m, T3 slightly worse.

**That is now the FOURTH mechanism to leave the clock episodes untouched**
(datum election on the prioritized path, handover-continuous, the pin bridge, and
the coverage gate). Four independent interventions on the phase-bias gauge, all
null on the clock channel, is strong evidence that **the clock churn is not a
phase-bias gauge problem at all** and that §5.05's third root lies elsewhere.

A two-pass epoch would differ from the bridge in one respect only: its target
would be the CURRENT epoch's solved value rather than the previous epoch's level,
and it would be a hard pin rather than a soft row. Given four null results on the
clock channel, its expected value there is low; it is not scheduled.

### 5.1 The real fix, not yet built

Both remedies above are avoidance. The defect itself is that a phase-bias column
removal is a **one-edge event**: the decoupling edge is unbounded (the clock
absorbs whatever the column held) and the re-coupling edge is implicit (each
station's ambiguity refloats, several epochs later, driven by RAIM rather than by
the removal). Invariant #7 says both edges must be explicit, counted, and
bounded. The correct fix keeps the network's shared phase-bias set from depending
on any single station's visibility at all — the set should follow *network*
coverage, with the pivot supplying only the gauge pin, and a pivot that loses a
satellite should hand that satellite's pin to another station rather than delete
the column. That is close to what `phase_datum_prioritized` attempts; its measured
harm here suggests the election needs the anti-churn treatment (sticky anchors,
hysteresis) before it is usable, not that the idea is wrong.

---

## 6. Relationship to the T3 false-alarm anomaly

This defect contributes to, but does not explain, the T3 post-fit RAIM
implied-α anomaly (`SPEC_sim_injection_bench.md` §5.5). Electing a clean datum
cuts the T3 flag rate 7.47 % → 4.49 %, so roughly **40 %** of the false alarms in
the worst arm are downstream of an avoidable gauge event. A **4.5 % floor
remains**, and that floor is the separate finding: the T3 statistic is a per-cell
**max over rows** normalized by a **median over individual rows**, then compared
against a Gaussian 6σ quantile. The implied-scale-versus-quantile curve rises
monotonically (1.83 → 3.18 → 4.49 → 6.14 → 313) with excess kurtosis 4391, so the
residue is shape, not scale — a threshold/population mismatch that datum election
cannot touch.

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
```

Configs: `_simnet24_inj_null_l13.json` (datum S12, `trace_station_residuals` on),
`_simnet24_inj_ds_S06000SIM.json`, `_simnet24_inj_ds_S13000SIM.json`,
`_simnet24_inj_null_pdp.json` (`phase_datum_prioritized` ON).
