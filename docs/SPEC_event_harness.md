# SPEC — the EVENT HARNESS (producer-side fault injection on a warm resume)

**Status: BUILT and VALIDATED on the sim quiet bed, 2026-09-10.** Default OFF and
structurally byte-identical (§7). Directive that motivated it: *"run the network to
convergence once, then inject one simulated event and continue a few epochs to study
the effects — minutes, not day-long runs."*

---

## 1. What it is

Grading a mechanism on this project has meant running the network for hours and
waiting for nature to supply an event. The turnaround for one question is a day, and
the answer is a mixture of the event, its neighbours, and the run's own ignition.

The harness inverts that:

1. **Converge ONCE** into a state snapshot (`scripts/event_leg.py write-seed`). One
   6 h run on the sim bed; snapshots every 60 emitted epochs.
2. **Resume for a few tens of epochs** with exactly ONE scripted fault armed
   (`network.inject`), and again with none (the NULL leg).
3. **Difference the two** (`scripts/event_grade.py`). Both legs resume from the same
   snapshot with the same config except `network.inject`, so the warm-resume seam
   transient, the bed and the posture are common and cancel.

Cost: **~2 min per leg** (61 epochs at 60 stations), against a 6 h seed you pay once.

It is **not** a solution producer. An armed leg's products are a harness fixture; the
loader and the run-start banner both say so in as many words.

## 2. The five injectable events

`network.inject.events` is a JSON array; `enable` defaults to true when the array is
non-empty. `tow` is GPS seconds-of-week at which the event starts (the run's week is
implied; a leg never spans a rollover), matched with a ±0.25 s half-epoch guard.

| kind | fields | what it does |
|---|---|---|
| `slip` | `station, sat, signal, tow, delta_cycles, flag_lli` | adds `delta_cycles` to that station's phase observable of (sat, signal), **from `tow` onward** — a STEP, not a ramp. `flag_lli` raises LLI bit 0x1 on the onset epoch only. |
| `outage` | `station, sat, tow, duration_s` | drops every observation of (station, sat) for `[tow, tow+duration_s)`. The return is a natural re-admission. |
| `anchor_handover` | `sat, signal ("all"\|code), tow, mode ("cold"\|"continuous")` | excludes the incumbent phase-anchor station from candidacy for that family for ONE epoch, forcing a successor. `cold` additionally refuses the continuous-handover branch, so the family takes the re-realize + re-reference path. |
| `arc_erase` | `station, sat, tow` [, `mode`, `signal`] | runs the T4 attribution refloat for that (station, sat) — appended to `bad_arcs` so it executes **the same code** a real attribution does (carry erase, iono-carry erase, pending-reseed, C3c queue).  The optional DIAGNOSTIC `mode` (§9.3) subtracts ONE of those actions, or (`free_epoch`) routes the event to a detector refloat instead; the optional `signal` is a `;`-separated BAND SCOPE.  Defaults (`"erase"`, every signal) are the production path. |
| `sat_clock_step` | `sat, tow, step_m` | adds `step_m` to the a-priori satellite clock used by every station, from `tow` on. |

### 2.1 Where the injection acts, and why it is one site

Four kinds are corruptions of the producer's INPUT. They are applied to a per-epoch
mutable copy of the `SyncedEpoch` at the very top of the epoch body
(`ApplicationNetwork.cpp`, immediately after the `start_offset_hours` gate) — before
`networkFastClockTick`, before `computeModels`, before every T0 detector, before
`networkComputeRawObservables`, before assembly.

That one site is sufficient because **both downstream representations are derived
from it**. `PppModel::computeModels` builds

* `PppObservation::observed` — phase in **metres** (`observed_value *= wavelength`),
  what `MeasurementModel` turns into design rows; and
* `PppObservation::raw_observed` — phase in **cycles**, what MW / GF / IF-TD read

from the same `RinexObservation::measurements` entry, which is in **cycles**. So a
single scalar write on the `RinexObservation` moves the slip detectors and the design
matrix together and consistently. Injecting into `modeled` instead would need a dual
write with a wavelength lookup, at a site inside an OpenMP region, plus a second site
for the legacy pivot — and would still miss the fast-clock tier, which reads
`se.by_station` directly.

`sat_clock_step` is applied at the same site and this is **exact, not an
approximation**. The uncombined residual is `y = L̃ − (ρ − c·δtˢ + …)`, so adding
`step_m` metres to every code observable and `step_m/λ` cycles to every phase
observable of a satellite **is** adding `step_m` to the a-priori satellite clock
`c·δtˢ` used by every station. Doing it here keeps the injector out of
`ModelGeometry`, which has no access to the run context.

The two decision-side kinds are staged as one-epoch directives in
`NetworkRunContext::inject` and consumed by the seam that owns the decision:
`anchor_handover` → `networkElectPhaseAnchors` (a candidacy **ban**, so the successor
is still chosen by the production rules — the harness forces the EVENT, never the
outcome); `arc_erase` → the T4 block in `networkPostFitRaim`.

### 2.2 Never-fired is a HARD error

Verbatim the simulator's scripted-slip rule (`ScriptedSlips::unfinished`,
`src/sim/main.cpp`): an armed event that never fired fails the run at run-end, by
name, with the three things to check (roster, obs code, epoch window). A harness whose
fixture silently did not exist is worse than one that refuses to finish — the leg
would otherwise be graded as a null and reported as "the event had no effect".

## 3. Files

| file | role |
|---|---|
| `include/filter/NetworkInject.hpp`, `src/core/filter/NetworkInject.cpp` | the seam: schedule, per-epoch application, staged directives, run-end clause |
| `include/config/GnssConfigTypes.hpp` | `NetworkInjectEvent`, `NetworkInjectConfig` |
| `src/core/config/GnssConfigLoadNetwork.cpp` | `loadNetworkInject_` (strict: per-index errors, 64-event cap, per-kind required fields) |
| `include/app/NetworkRunContext.hpp` | `NetworkInjectState inject` |
| `src/core/app/ApplicationNetwork.cpp` | arm + the per-epoch seam + the anchor-handover readback |
| `src/core/filter/NetworkPhaseDatum.{hpp,cpp}` | the anchor candidacy ban + forced-cold branch |
| `src/core/filter/NetworkRaim.cpp` | the injected T4 erase |
| `src/core/filter/NetworkTeardown.cpp` | run-end clause + the never-fired exit code |
| `src/core/logging/NetworkRunTraces.cpp` | manifest keys (armed-only) |
| `scripts/event_leg.py` | `write-seed` / `make-leg` / `null` / `rerun` |
| `scripts/event_grade.py` | the event-minus-null grader |
| `include/slip/NetworkSlipQc.hpp`, `src/core/slip/NetworkSlipQc.cpp` | §9: `networkSlipDrainInjectedFree` — the injected `free_epoch` response (a PASS-A drain, beside the real detectors) |
| `src/core/app/ApplicationNetworkAssembly.cpp` | §9: the drain's call site (after the slip drains, before the PASS-B leveling inject) |
| `scripts/ev_study1_legs.py`, `scripts/ev_study1_table.py` | §9: the Study-1 leg matrix + summary table |
| `scripts/ev_launch.ps1` | detached launcher (per-run cwd, always writes `exitcode.txt`) |
| `tests/test_network_inject.cpp` | 23 cases: parse contract + epoch semantics |

## 4. The leg recipe

```powershell
# ONE seed (6 h, ~40 min at 60 stations; snapshots at emitted epochs 1, 61, 121, ...)
python scripts\event_leg.py write-seed --name quiet6h --hours 6
pwsh scripts\ev_launch.ps1 -Exe <release exe> -Config runs\evseed_quiet6h\seed.json `
                           -RunDir runs\evseed_quiet6h        # detached; poll exitcode.txt

# the NULL control (every event leg is graded against this)
python scripts\event_leg.py null --seed-run runs/evseed_quiet6h --snapshot-ep 721 `
       --epochs 60 --name null --run

# one event, at +10 epochs (past the ~5-epoch resume transient)
python scripts\event_leg.py make-leg --seed-run runs/evseed_quiet6h --snapshot-ep 721 `
       --epochs 60 --name slip10 --at-ep 10 --run `
       --event '{"kind":"slip","station":"S12000SIM","sat":"G05","signal":"L1C","delta_cycles":10}'

python scripts\event_grade.py runs/evleg_slip10 --null runs/evleg_null `
       --fixture C:/GnssProcessor/runs/simnet60_quiet_wu93_24h
```

`make-leg` copies the chosen `.epNNNN` snapshot to `runs/evleg_<name>/seed.txt` (never
the seed's live file — a killed leg must not be able to destroy the state it resumed
from), derives `network.start_offset_hours` from the snapshot's `M` line with the
half-epoch back-off `(week·604800 + tow − 1399161600 + 15)/3600`, sets
`processing.max_duration_s = 30·(epochs+1)` (anchored on the first PROCESSED epoch —
the offset gate returns before the duration gate is reached), and arms the leg-E
warm-restart posture: `state_snapshot_require`, `resume_frame_restore`,
`resume_coupling_restore` on a writer that captured frame + coupling (QK/QP).

⚠ **A leg is not re-runnable in place** — it overwrites its own `seed.txt` at its
first emitted epoch (§6b). Use `python scripts/event_leg.py rerun <legdir>`, which
re-seeds from the pristine `.epNNNN` first.

`--at-ep` counts from the leg's FIRST processed epoch, so the default 10 puts the
event past the ~5-epoch resume transient that `SPEC_synthetic_minimum_network` §18.14
tells you to discard.

## 5. Reading the grader

Rows are epochs relative to t0. Columns: `lvl` (the injected arc's chord-median DD
level error in cycles, with the injected delta folded into the truth so a perfect
response reads ~0), `zwd` (station ZWD error vs `truth_tropo.csv`), `clk_d`/`pb_d`
(the emitted satellite clock and phase bias **minus the null leg's** — the wire
signature), `cons` (consumer 3D error and `ar_gate`), and `qc` (tier/event rows from
`network_events.csv` on that (station, sat)). `_d` columns are event − null.

⚠ **`lvl` inherits the DD trace's single reference satellite** (`SPEC_amb_iono` §6.12):
its own between-station error is in every chord. Read the DIFF column, which cancels
it wherever the two legs share the reference — they do, except where the event itself
moves the reference.

## 6. Validation, 2026-09-10

Seed `runs/evseed_quiet6h`: `golden_sim_tri_w6` on `simnet60_quiet_wu93_24h`, 60
stations + 1 held-out consumer (SZZ000SIM), GPS+Galileo triple, 6 h = 721 epochs,
**1540 s wall**, exit 0. Snapshots at emitted epochs 1, 61, …, 665.

Every leg below: 61 epochs from `state_snapshot.txt.ep0665` (tow 279150), event at
+10, graded against `runs/evleg_null`. **Wall time 148–151 s per leg** (four legs
concurrently; 105 s alone). Target arc `S21000SIM / G10 / L1C` — a mature arc, not
the clock-datum station (S30000SIM), not the consumer, and G10 is not the DD trace's
reference satellite (G01 / E02).

### (1) The seam transient — and why grading is event-minus-null

`runs/evleg_seam` (a NULL leg from ep0061) against the continuous seed run over the
same 60 epochs, `event_grade.py --seam-ref`:

| leg epoch | max abs dC (m) | rms dC | max abs dP (m) | rms dP |
|---|---|---|---|---|
| +0 | 0.1323 | 0.0224 | 0.1231 | 0.0329 |
| +9 | 0.1420 | 0.0255 | 0.3303 | 0.1330 |
| +18 | 0.1437 | 0.0263 | 0.4727 | 0.1864 |
| +19 | 0.1435 | 0.0263 | **10.587** | 0.869 |
| +59 | 0.1440 | 0.0298 | **20.521** | 2.361 |

**The raw wire NEVER settles below 1 cm on either channel.** The clock channel is
flat (max 0.13 -> 0.14 m, rms 0.022 -> 0.030 m over 60 epochs — it does not decay,
it does not grow); the phase-bias channel drifts to ~0.5 m and then takes two
discrete re-realization steps (+19 and later) to 20.5 m max / 2.4 m rms.

Those P steps are a **frame** difference, not an error — standing invariant #5's
exact warning about grading raw wire steps. The consequence for the harness is the
design decision it validates: **every event leg is graded against a NULL leg resumed
from the same snapshot, never against the continuous run.** Both legs carry this
transient identically and the `_d` columns cancel it. The measured event signatures
below are 0.0001–0.5 m on channels whose seam transient is 0.14–20 m; without the
null they would be unreadable.

### (2) `slip`, 10 cycles on S21000SIM/G10/L1C

* **Detected at +0** — the same epoch, zero latency — by `T1_if_slip/if_slip`,
  statistic **4.876 against gate 0.612 (8.0x)**, action `refloat` on `L1C;L2W`. The
  NULL leg has no event on that station in the window, so the row is entirely the
  injected slip's.
* Arc level error (event - null), mean abs by band:
  **+1..+5 0.029 cyc, +6..+10 0.043, +11..+20 0.048, +21..+30 0.048, +31..+45 0.040.**
  A 10-cycle step leaves a **~0.04 cycle** residual from the first epoch after the
  response, flat thereafter. The response is correct.
* Station ZWD error (event - null) peaks **+1.4 mm** at +38; wire C step **+0.4 mm**,
  P(L1C) **+0.1 mm**; consumer 3D (event - null) **-0.001 m**.

### (3) `arc_erase` on the SAME arc — the re-level treadmill, priced

A T4 erase injected on a **healthy** arc, executing the production attribution path
(`refloated 3 carried chord(s)`, sigma0 = 1.000, event row `T4_attrib/attrib_arc`):

| band | mean abs level error, event - null (cyc) | slip leg, same bands |
|---|---|---|
| +1..+5 | **1.194** | 0.029 |
| +6..+10 | **1.314** | 0.043 |
| +11..+20 | **0.508** | 0.048 |
| +21..+30 | **0.452** | 0.048 |
| +31..+45 | **0.454** | 0.040 |

Peak **-1.905 cyc at +2**; it decays to a **persistent ~0.45 cycle floor by +14 and
stays there for the remaining 31 epochs**. Station ZWD error peaks **+3.8 mm** at +31
(2.7x the slip leg's). Wire C **+0.7 mm**, P(L1C) **+0.2 mm**; consumer -0.001 m.

**One unnecessary erase costs ~11x the residual level error of a correctly handled
real 10-cycle slip, and it does not go away.** That is the re-level treadmill
(standing invariant #1's re-coupling edge, `SPEC_slip_relevel_walk`) isolated to a
single event and measured in 148 s, on a bed with no slip at all.

### (4) `anchor_handover` on G10 — cold vs continuous, 50x

Both modes retired the same incumbent and elected the same successor, so the only
variable is the branch:

```
INJECT,anchor_handover,G10,,3.000000,,injected,mode=cold;families=3;forced_cold=3;
       retired=S29000SIM|S29000SIM|S29000SIM;elected=S06000SIM|S06000SIM|S06000SIM
INJECT,anchor_handover,G10,,3.000000,,injected,mode=continuous;families=3;forced_cold=0;
       retired=S29000SIM|S29000SIM|S29000SIM;elected=S06000SIM|S06000SIM|S06000SIM
```

| | wire P(L1C) step (event - null) | consumer 3D (event - null) |
|---|---|---|
| **cold** | **+20.079 m at +0** | **-0.546 m at +45** (still growing) |
| **continuous** | **+0.403 m at +45** (0.001 m at +0) | +0.084 m at +1, ~0 after |

**50x on the wire, and the consumer difference is the point**: the cold re-realization
is a metre-class consumer error that has not recovered 45 epochs later, while the
continuous handover is a centimetre-class one-epoch blip. This reproduces the
single-pivot PB gauge defect and the value of the two-edge continuous handover as a
controlled A/B, in 300 s of wall clock.

### (5) `outage`, 20 epochs on S21000SIM/G10

Ledger exact: `outage_begin;dropped=1` at t0, `released;epochs_dropped=20` at +20 —
the window is exactly the 600 s asked for. Effects:

* **The leveling-carry retirement census moves by exactly +3** (264 -> 267 permanent
  amb-carry prunings) — the three signals L1C/L2W/L5Q of that arc, pruned on the
  120 s gap rule.
* **`READMIT continuity` reports 0 re-admissions**, correctly: `readmit_seed_horizon_s`
  tracks satellites departing the NETWORK, and G10 stayed visible at 59 other stations.
* **`network_events.csv` carries NOTHING for that arc at either edge** — no detector
  row at the drop, no row at the return. The decoupling edge is counted only in a
  run-end census and the re-coupling edge is a fresh birth with no re-validation.
  That is standing invariant #7's named failure mode, on a per-station arc gap.
* Station ZWD error peaks **+4.4 mm** at +31; wire C **+1.1 mm**, P(L1C) **+0.5 mm**;
  consumer 3D **+0.005 m** at +37; arc level error back under 0.5 cyc from +27.

### (6) `sat_clock_step`, +0.5 m on G10

The emitted clock absorbs the step **within one epoch and essentially in full**:

```
ep      -1      +0      +1      +2      +5     +10     +15
dC(m) 0.0000 -0.5006 -0.5009 -0.4993 -0.5009 -0.5021 -0.5022
```

i.e. **100.4 %** of the injected 0.5 m, settling at -0.5022 m; P(L1C) moves **-0.6 mm**
— the step lands where it physically belongs and nowhere else. The clock temporal
anchor both detected and signalled it:

```
CLK_ANCHOR,cta_decouple,G10,,11.080647,6.000000,refloat,reason=nis;e=-0.2188;...
CLK_ANCHOR,cta_disc_landed,G10,,,,disc_bump,clock_anchor_decoupled
CLK_ANCHOR,cta_recouple,G10,,0.004518,6.000000,reengage,in_band=4;reseed_var=1.511e-05
```

NIS **11.08 against gate 6.0** at +0, a discontinuity bump raised for the consumer,
and re-coupling at **+4 epochs** after 4 in-band epochs — both edges instrumented.
Consumer 3D (event - null) -0.029 m at +0.

### Byte-identity and the suite

* **Golden 61-epoch smoke, same build config, products.txt SHA256**
  `3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357` from BOTH the
  new exe and the main-HEAD `gnss_processor_oracle2.exe`
  (`98B2CB21D1BB7EF76652D3D2AC099D5EAB6F1AECEDB43D6104358887CCC500CA`). Identical.
* **Debug suite: 1713 cases / 241 suites, 1696 passed, 0 failed, 17 skipped**
  (16 of those are fixture-gated — `data/` is not bootstrapped in this worktree).
  23 of the cases are this card's.

## 6b. A defect the harness found on its first day

**`state_snapshot_require: true` does NOT hard-fail on the non-positive-age refusal.**
A leg writes its own snapshot at its **first emitted epoch** —
`state_snapshot_interval_s` does not suppress that first write — so after one run
`seed.txt` holds the leg's own second epoch. Relaunching the exe on the same
`leg.json` then hits `warmResumeAllowed`'s "first live epoch is not after the
snapshot epoch (age 0.000 s)" branch, which is **not** wired to the HB-1 hook
(`NetworkRunContext.cpp:1018-1030`): it warns, falls through to a COLD START, and the
run then died `0xC0000409` 25.8 s later. `SPEC_warm_resume_event_card` §0.4 states
this trap is "closed structurally ... by `network.state_snapshot_require`"; for this
refusal reason it is not.

Harness-side mitigation: **`python scripts/event_leg.py rerun <legdir>`** restores the
pristine snapshot from `leg_meta.json` before relaunching. Never invoke the exe on an
existing `leg.json` directly. The producer-side fix (route this refusal through
`hb1()`) is left as its own card — it is not the injector's.

## 7. Byte-identity

`network.inject` absent (or `enable` false, or an empty `events` array) leaves
`NetworkInjectState::armed` false. `networkInjectEpoch` then returns a reference to
the caller's own `SyncedEpoch` — **no copy is taken at all** — the anchor election and
the RAIM seam see a null/empty directive, and the manifest keys are emitted only when
armed (the `mdb` / `adm_screen` precedent, because the decomposition golden hashes
that file).

## 8. Known gaps — what is NOT injectable

* **Only the five kinds above.** Notably absent: multipath, a station-wide outage, an
  ephemeris/orbit error, a receiver clock jump, a troposphere step, an antenna change,
  and a *partial* signal degradation (the harness drops or shifts, it does not
  deweight).
* **`arc_erase` mode / band scope are HARNESS-ONLY.**  They gate what the
  INJECTED erase does; a real attribution takes the production path on every
  mode, and an unarmed run reaches none of it (§9.3).  Two of the four modes
  (`erase_keep_iono`, `erase_no_funnel`) are structurally inert on the sim
  golden's posture and byte-identical to `erase` there — see §9.4.
* **`arc_erase` rides the RAIM seam and its attribution pass.** Three conditions
  must hold: the seam runs (`raim_on && (carry_warm || raim_ci_on)`), the attribution
  cell pass runs (`need_attrib = raim_attribution || trace_flag_rates`), and the cell
  pool is non-empty. The goldens satisfy all three. Where one is closed the event
  cannot fire — and, correctly, the run then fails at run-end rather than reporting a
  null. Both decision-side kinds RE-STAGE their directive every epoch until the owning
  seam actually acts, so the event fires at the first epoch at or after `tow` where
  that seam runs; the event row records the epoch it really landed on.
* **`anchor_handover` is a one-epoch ban.** The retired incumbent is eligible again
  the next epoch; whether it is re-elected is the sticky rule's business, not the
  harness's. There is no "retire for N epochs" form.
* **`sat_clock_step` is a step, not a drift.** A rate would need a second field and a
  per-epoch accumulation; the algebra is the same.
* **The injector is outside the warm-resume fingerprint and the snapshot** — by
  design (two legs off one snapshot must differ ONLY in their schedule). The
  consequence is that a leg that itself writes a snapshot passes NO record of its
  corruption downstream. Do not chain legs.
* **Slip repair is not modelled.** A `slip` shifts the observable forever; the
  producer's response is what is being measured. There is no "the receiver
  re-acquires cleanly at +N" form.

## 9. Study 1 — erase vs refloat: what actually costs the 0.45-cycle floor

**Answer, up front: none of the four actions the T4 erase performs and a detector
refloat does not. The floor is bought by the BAND SCOPE — freeing (or erasing)
ALL of a satellite's signals at one station at once. Restrict either path to 2 of
3 bands and the 0.445-cycle floor becomes 0.050.** Measured 2026-09-10, 14 legs,
~14 min of wall clock.

### 9.1 The question

§6(2) and §6(3) measured, on the SAME healthy arc, that a detector-handled
10-cycle `slip` leaves ~0.04 cyc of residual level error while an `arc_erase`
leaves a peak −1.9 cyc and a persistent ~0.45 cyc floor from +14 through +45 —
11×, with ZWD +3.8 mm. On the slipped storm bed the attribution tier erases ~21
arcs per station-hour, so that floor is the ambient 1.7-cycle plateau priced per
event. Which extra thing does the erase do?

### 9.2 The exact difference list (read from the source, not inferred)

`refloat_sat_at` in `src/core/filter/NetworkRaim.cpp` (~:751-880), per matching
ambiguity of the flagged (station, satellite):

| # | action | T4 erase | detector refloat (`refloat_signal`, `slip/NetworkSlipQc.cpp` ~:415-470) |
|---|---|---|---|
| 1 | `LocalCarry` entry | **`local_carry.erase(...)`** — deleted; the re-stash is a NEW entry (birth reset, `var_first` reset, F3 block membership restarts) | **KEPT** |
| 2 | LOS iono carry | **`ctx.carry.iono_carry.erase({i, sat})`** (`n_iono_carry_erased_t4`) | not touched |
| 3 | pending-reseed | `markAmbiguityPendingReseed(amb_key)` | `markAmbiguityPendingReseed(amb_key)` — **the same call** |
| 4 | C3c funnel | `raim_refloat_pending.insert(...)` under `raim_ci_refloat`, consumed NEXT epoch | `carry_slipped_ambs.insert(...)` under `carry_invariant`, consumed THIS epoch |
| 5 | free-epoch latch | **none** — the RAIM path never touches `free_ambs` | **`relevel->free_ambs.insert(...)` + `free_cause`** — the set the PASS-B leveling inject actually reads |
| 6 | bridge pend | none | voids any outstanding `relevel_pend` on that (station, sat) |
| 7 | seam / timing | POST-fit (`networkPostFitRaim`), so the event epoch's own solve is untouched and the damage starts at +1 | PASS A, so the event epoch's own solve is already freed (visible at +0) |

Two of those are not differences at all: **(3) is the same call on both paths**,
and **(4) is the same idea on two vehicles**. The real asymmetry is (1) vs (5):
the erase *removes the level*, the refloat *keeps the level and skips injecting it
for one epoch*. Both end with the arc re-levelled from one epoch of data — which
is the point the measurement lands on.

### 9.3 The diagnostic sub-knobs (harness-only)

`network.inject.events[].mode` on kind `arc_erase`, default `"erase"` = the
production path, so an event that names no mode is exactly what it was before:

| mode | LocalCarry | LOS iono | pending-reseed | C3c queue | free-epoch latch | seam |
|---|---|---|---|---|---|---|
| `erase` (default) | ERASED | co-erased | marked | queued | no | post-fit RAIM |
| `erase_keep_iono` | ERASED | **KEPT** | marked | queued | no | post-fit RAIM |
| `erase_no_reseed` | ERASED | co-erased | **NOT** | queued | no | post-fit RAIM |
| `erase_no_funnel` | ERASED | co-erased | marked | **NOT** | no | post-fit RAIM |
| `free_epoch` | **KEPT** | KEPT | marked | detector's | **YES** | PASS-A drain |

Plus an optional BAND SCOPE: `"signal": "L1C;L2W"` restricts the action to those
signals (empty / `"all"` = every signal, which is what BOTH production paths do).

`free_epoch` is routed to a new PASS-A drain, `networkSlipDrainInjectedFree`
(`slip/NetworkSlipQc.{hpp,cpp}`, called from `ApplicationNetworkAssembly.cpp`
beside the other slip drains). It has to live there, not in the RAIM block:
`relevel.free_ambs` is cleared at the top of PASS A and read by the PASS-B
leveling inject, so a `free_ambs` insert from post-fit RAIM would never be seen.
Run-end census `Study-1 path census (mode=chords acted)` says which path ran —
invariant #4.

### 9.4 The legs

Seed `runs/evseed_quiet6h` `state_snapshot.txt.ep0665` (tow 279150), 61 epochs,
event at +10 (tow 279480), everything graded against the ONE null leg
`runs/evleg_null`. Three arcs, all GPS triple (L1C / L2W / L5Q):

| arc | station / sat | mean elevation |
|---|---|---|
| arc 1 | `S21000SIM` / `G10` (the §6 validation arc) | 14.2° |
| arc 2 | `S50000SIM` / `G31` | **81.3°** |
| arc 3 | `S40000SIM` / `G31` (arc 2's satellite, low station) | 13.8° |

**Two structural gates first.**

* **Byte-identity (invariant #6).** `runs/evleg_s1_nullgate` — a NULL leg on the
  new exe — reproduces `runs/evleg_null`'s `products.txt` SHA256 exactly
  (`95DC556F1EDBB6EBB06D2E6BDC03282F9169887C63BBCEF0…`). The sub-knobs are
  unreachable unarmed, and the §6 validation legs stay comparable.
* **Two of the four actions are STRUCTURALLY INERT on this lineage, and the
  harness proves it rather than assuming it.** `erase_keep_iono` and
  `erase_no_funnel` are **byte-identical to `erase`** (`products.txt`
  `7027918C0DE61D0E11C67646AB023A1179B2AB03…` for all three), because the sim
  golden runs `carry_iono: false` (⇒ `ctx.knobs.iono_on` false, so there is no
  iono carry to co-erase) and `carry_invariant: false` (⇒ `raim_ci_refloat`
  false, so nothing is queued). **The LOS-iono co-erase and the C3c funnel
  therefore CANNOT be the cause of the measured floor** — they never execute.
  What their arms certify is the mode dispatch: exactly the named action moved
  and nothing else did. On a `carry_iono` / `carry_invariant` lineage they are
  live and untested — that is an owed leg, not a closed question.

### 9.5 The table

Arc level error is the chord-median DD error of the arc, **event − null**, in
cycles, from `logs/network_dd_fractional.csv` joined to the fixture's true
integers (`scripts/ev_study1_table.py`). `floor` = mean |e| over +14..+45. ZWD
and iono are event − null too; iono is `iono_total_m` for that (station, sat)
at +45. `T4/T3` counts post-event re-flags on the arc inside 45 epochs.

| leg | peak \|e\| (cyc) | @ep | +14 | +30 | +45 | **floor** | ZWD pk (mm) | Δiono +45 (m) | T4/T3 | cons pk (m) |
|---|---|---|---|---|---|---|---|---|---|---|
| **arc1** `slip` 10 cyc (detector freed L1C;L2W) | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc1 `erase` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_keep_iono` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_reseed` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `erase_no_funnel` | 1.905 | +2 | 0.415 | 0.588 | 0.417 | **0.445** | +3.8 | −0.0832 | 0 | −0.001 |
| arc1 `free_epoch` (all 3 bands) | 2.018 | +0 | 0.515 | 0.633 | 0.447 | **0.496** | +4.0 | −0.0890 | 0 | −0.001 |
| **arc1 `erase` scoped L1C;L2W** | 0.067 | +1 | 0.059 | 0.050 | 0.041 | **0.050** | −1.9 | +0.0061 | 0 | −0.001 |
| **arc1 `free_epoch` scoped L1C;L2W** | 0.054 | +11 | 0.051 | 0.044 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | −0.001 |
| arc2 `erase` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `erase_no_reseed` | 1.052 | +1 | 0.270 | 0.067 | 0.073 | **0.130** | −4.2 | +0.0206 | 0 | −0.011 |
| arc2 `free_epoch` (all 3) | 0.434 | +1 | 0.213 | 0.088 | 0.077 | **0.116** | −3.8 | +0.0221 | 0 | −0.012 |
| **arc2 `free_epoch` scoped L1C;L2W** | 0.019 | +1 | 0.007 | 0.004 | 0.003 | **0.004** | −1.8 | −0.0004 | 0 | −0.005 |
| arc3 `erase` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `erase_no_reseed` | 0.803 | +1 | 0.448 | 0.379 | 0.207 | **0.375** | −2.0 | +0.0405 | 0 | −0.001 |
| arc3 `free_epoch` (all 3) | 0.498 | +10 | 0.372 | 0.325 | 0.179 | **0.319** | −2.5 | +0.0353 | 0 | −0.001 |

Per band on arc 1 (floor +14..+45), which is where the mechanism is visible:

| band | slip (det. L1C;L2W) | erase all-3 | free all-3 | erase L1C;L2W | free L1C;L2W |
|---|---|---|---|---|---|
| L1C | 0.044 | 0.445 | 0.496 | 0.050 | **0.044** |
| L2W | 0.052 | 0.598 | 0.664 | 0.061 | **0.052** |
| L5Q | 0.042 | 0.630 | 0.698 | 0.046 | **0.042** |

### 9.6 Verdict

1. **No single erase-path action produces the floor.** Removing the iono
   co-erase, the C3c queue, or the pending-reseed mark leaves the floor at 0.445
   on arc 1 (the first two byte-identically; `erase_no_reseed`'s products differ
   but every graded column agrees to three decimals). Erasing the `LocalCarry`
   entry is not it either: **`free_epoch` — a literal detector refloat that keeps
   the entry, keeps the iono, queues nothing and latches the free epoch —
   reproduces the floor and is marginally WORSE on arc 1 (0.496 vs 0.445).** The
   premise "the erase path is the expensive one" is falsified. Both paths cost
   the same because both end in the same place: the arc re-levelled from one
   epoch of data.
2. **The variable that matters is BAND SCOPE.** Restricting either path to 2 of
   the 3 bands drops the floor 9× (erase 0.445 → 0.050) and 11× (free 0.496 →
   0.044) on arc 1, 29× on arc 2 (0.116 → 0.004), and does it on all three bands
   at once — including L5Q, which was never freed. The scoped `free_epoch` leg
   reproduces the real-`slip` leg to three decimals on every band (0.044 / 0.052
   / 0.042), which is the identity check the mechanism predicts: the §6(2) slip
   was cheap **because its `T1_if_slip` detector acted on `L1C;L2W` and left L5Q
   pinned**, not because a slip response is intrinsically gentler than an erase.
3. **The mechanism.** Freeing every band of a LOS removes that LOS's last level
   anchor, so (ρ, I, N₁, N₂, N₃) must be re-drawn together from one epoch of
   code — the amb/iono split over-count (`SPEC_amb_iono` F1) at its worst, and
   standing invariant #1's re-coupling edge verbatim: *"a refloat that re-levels
   from ONE epoch of code CREATES a fresh zero-q value seeded with
   station-common iono error."* The iono column is the direct witness: −0.083 m
   of slant-iono error still standing at +45 on the all-3 arms against +0.006 m
   scoped. Leave ONE band pinned and the two re-tapped levels land on the
   survivor's ionosphere instead of on code noise.
4. **It scales with elevation, not with the path.** floor(erase): 81.3° arc
   0.130, 13.8° arc 0.375, 14.2° arc 0.445 — the one-epoch re-level is only as
   good as that epoch's code, and a low LOS has the worse code.
5. **The QC tiers stay silent.** 0 post-event T4/T3 re-flags on the arc in every
   leg, on both paths: the arc that has just been given a metre-class level error
   is not flagged for it. Invariant #7's re-coupling edge with no instrument.
6. Consumer 3D (event − null) is ≤ 12 mm on every leg, so one event is a wire
   defect long before it is a consumer defect; the plateau is the accumulation of
   many.

**The production change this points at** (not made here — the sub-knobs are
injector-only): band-scope the T4 attribution erase and the whole-satellite
detector refloats — refloat the implicated bands and leave the least-suspect
band's level pinned as the LOS's iono anchor — which is the "band-scoped
refloat" already named as the open build in `SPEC_lane_slip_response`.

### 9.7 Reproducing

```powershell
python scripts\ev_study1_legs.py make                       # 14 leg.json
python scripts\ev_study1_legs.py run  s1_free s1_keepiono s1_noreseed   # <=3 at a time
python scripts\ev_study1_table.py                           # the table above
```

Wall clock: 150–163 s per leg with 3 concurrent (211 s in the one wave that
overlapped another session's build), 14 legs in ~14 min; Release build 379 s
(+123 s incremental for the band scope), Debug suite 367 s.
Unit suite after the card: **1716 cases / 241 suites, 1699 passed, 0 failed, 17
skipped** (fixture-gated; `data/` is not bootstrapped in this worktree) — the
card adds one epoch-semantics case (`ArcEraseStudy1ModeAndBandScope`) and two
parse-rejection entries.

**Caveats.** (a) One quiet sim bed, three arcs, 60-epoch legs — the floor is
measured out to +45, not to convergence. (b) The iono co-erase and the C3c
funnel are inert here; their arms are certification of the dispatch, not of the
actions. (c) `mode` gates the INJECTED erase only; a real attribution takes the
production path on every mode, and an unarmed run cannot reach any of it.
(d) The `free_epoch` legs move at +0 and the erase legs at +1 — that is the two
seams' real timing (PASS-A drain vs post-fit RAIM), not a grading offset.

## Z. Study 3 — band-scoped responses, the production knobs

**Answer, up front: the T4 half is a large, consistent win and the detector half
is not. `network.raim_erase_band_scope: min_keep_one` drops the level floor an
unnecessary attribution erase leaves by 9x / 29x / 64x on the three Study-1 arcs
(0.445 -> 0.049, 0.375 -> 0.013, 0.130 -> 0.002 cyc) and takes the standing slant
iono error with it (-0.083 -> +0.006 m at +45).
`network.cycle_slip.refloat_band_scope: implicated` is byte-exact on a
correctly-scoped single-band slip, but on a GENUINE all-band slip its fallback
pins a band that really moved and the arc is 15x WORSE (floor 0.498 -> 7.658 cyc,
slant iono -0.089 -> -1.302 m). Arm the erase scope; leave the detector scope OFF
until the lane-preserving refloat (`SPEC_lane_slip_response` §7) can re-level the
kept band instead of trusting it.  The 6 h storm-bed arm that carries BOTH (Z.6)
takes 30 % off the ZWD settled truth rms and loses 29 points of settled WL — the
split the leg matrix predicts, on a bed where the detector half pinned a
genuinely-slipped band 1 091 times.** Measured 2026-09-10, 7 legs + 1 storm arm.

### Z.1 What was built

Study 1 (§9) ended by naming the production change it pointed at and deliberately
not making it: the sub-knobs it used were injector-only. §Z is that change.

| knob | values | acts on |
|---|---|---|
| `network.raim_erase_band_scope` | `all` (default) / `min_keep_one` | the T4 attribution erase, `refloat_sat_at(..., statistical_arc=true)` in `NetworkRaim.cpp` |
| `network.cycle_slip.refloat_band_scope` | `all` (default) / `implicated` | the whole-satellite detector refloats in `NetworkSlipQc.cpp` — MW, LLI, the IF-TD pair, the wind-up restart drain |

Both default to today's behaviour and both are **solve-affecting when armed**, so
each is emitted into `run_manifest.json` only when armed (the `mdb` / `adm_screen`
precedent) and each appears in the acting event's `network_events.csv` detail as
`band_scope=...;kept=<band>`.

**`min_keep_one`.** Erase every band of the flagged (station, satellite) whose OWN
post-fit normalized residual |r|/sigma0 exceeds the T4 gate `k*sigma0`, and ALWAYS
spare the band with the smallest |r|/sigma0. Two edges are named rather than
assumed: when NO band clears the gate (the cell was flagged by the median-polish
INTERACTION, which is net of the row and column effects) everything but the pin is
still erased — absence of per-band evidence never suppresses the response
(invariant #1); when EVERY band clears it the pin is the least-BAD band and
`kept_over_gate` counts it. A single-band cell is skipped outright: there is no
band to spare, and freeing the only one is exactly the failure the scope exists to
prevent.

*Per-signal residual source.* `cells`, built in Pass 1 of `networkPostFitRaim` as
one row per **(station, satellite, SIGNAL)** carrying that carried ambiguity's max
standardized post-fit phase residual — the same number the median polish consumes
after `cell_max` collapses the signal axis away. Nothing is re-derived; the scope
reads the value the attribution was computed from, one aggregation step earlier.
The CONSENSUS whole-satellite path (`statistical_arc=false`) is deliberately NOT
scoped: a >= min_sta cross-station agreement is evidence about the satellite, not
about one arc's bands.

**`implicated`.** Free only the bands the detector named; when they cover every
live band of the arc, keep the least-suspect one pinned. "Least suspect" is the
MW/GF decomposition of the arc's primary pair: dN1 - dN2 = dMW/lambda_WL and
lambda1*dN1 - lambda2*dN2 = dGF separate exactly, so
dN1 = (dGF - lambda2*dMW)/(lambda1-lambda2) and
dN2 = (dGF - lambda1*dMW)/(lambda1-lambda2). Bands off that pair carry no evidence
and never out-rank a measured one; with no usable evidence the pin falls to the
lexically lowest survivor (deterministic, and the shortest wavelength). A
single-band arc is never spared here — unlike T4 the detector has POSITIVE slip
evidence, and with one band there is no LOS anchor to preserve anyway.

### Z.2 The defect the harness found on its first ON leg: the invariant is per EPOCH

The first build scoped each drain independently. `runs/evleg_z_slip3_on` (an LLI
on all three bands) then came out **byte-identical to its control** with the
counters reading `1 arc -> 2 freed / 1 kept`. The trace says why:

```
T0_mw_detect,lli_slip,G10,S21000SIM,refloat,LLI;band_scope=implicated;kept=L1C
T1_if_slip,  if_slip, G10,S21000SIM,refloat,L1C;L2W
```

The MW/LLI drain kept L1C. The IF-TD driver then implicated (L1C, L2W) — which
does **not** cover a 3-band arc, so its own scope is correctly a no-op — and freed
L1C anyway. The arc ended the epoch with every band freed, exactly as before.

**A per-drain scope cannot hold a per-arc invariant when several drains act on the
same arc in one epoch.** The decision is now taken against this drain's bands
UNION what earlier drains have already latched this epoch (read from
`relevel->free_ambs` and the Card-4 `carry_slipped_ambs` — both, because which one
is live is a posture decision the scope must not depend on), and the survivor is
chosen only among bands that are still pinned. A `bscope_too_late` counter records
the case where every band was already gone before the drain ran: nonzero means a
freeing site upstream is not routed through the scope, which is invariant #4
applied to the scope's own coverage. It is 0 on every leg here.

This also showed that **the detector scope is NOT inert on a quiet bed**: the
null-ON leg took 20 decisions in 61 epochs, because MW fires on (L1C,L5Q) and
(L1C,L2W) of the same satellite in one epoch and their union covers the arc. That
is a real, correct trigger — and it means **every ON arm must be graded against an
ON null**, which is how the table below is built. Graded against the OFF null the
ON rows carried a pre-event |e| peak at ep -1 and an identical +0.021 m consumer
offset: the background, not the event. (The one-per-satellite MW event row also
hid those keeps entirely; a dedicated `T0_mw_detect / band_scope / keep` row now
carries them — invariant #7 wants an identity at the edge, not a counter alone.)

### Z.3 The legs

Seed `runs/evseed_quiet6h` `state_snapshot.txt.ep0665` (tow 279150), 61 epochs,
event at +10, exactly the §6/§9 geometry. OFF arms are the §6/§9 legs already on
disk; ON arms are `runs/evleg_z_*` (`scripts/ev_study3_legs.py`). Each arm is
graded against the null of its OWN posture (`scripts/ev_study3_table.py`).

**Structural gate first (invariant #6).** `_golden_sim_off60_bscope.json` — the
golden 61-epoch smoke with both knobs ABSENT — reproduces `products.txt` SHA256
`3BF7831B29916C7A1E20434AC91F2ADFEB74EB06974560DF6E050FC43AC3F357`, the pre-card
hash, on the new exe. Unarmed, the new code is unreachable.

| leg | peak \|e\| (cyc) | @ep | +14 | +45 | **floor** | ZWD pk (mm) | dIono +45 (m) | T4/T3 | cons pk (m) |
|---|---|---|---|---|---|---|---|---|---|
| arc1 T4 erase 14.2deg **OFF** | 1.905 | +2 | 0.415 | 0.417 | **0.445** | +3.8 | -0.0832 | 0 | -0.001 |
| arc1 T4 erase 14.2deg **ON** | 0.067 | +1 | 0.059 | 0.041 | **0.049** | -1.9 | +0.0061 | 0 | -0.001 |
| | | | | | **x8.99** | | | | |
| arc2 T4 erase 81.3deg **OFF** | 1.052 | +1 | 0.270 | 0.073 | **0.130** | -4.2 | +0.0206 | 0 | -0.011 |
| arc2 T4 erase 81.3deg **ON** | 0.003 | +19 | 0.001 | 0.002 | **0.002** | -1.6 | -0.0003 | 0 | -0.002 |
| | | | | | **x63.53** | | | | |
| arc3 T4 erase 13.8deg **OFF** | 0.803 | +1 | 0.448 | 0.207 | **0.375** | -2.0 | +0.0405 | 0 | -0.001 |
| arc3 T4 erase 13.8deg **ON** | 0.061 | +3 | 0.035 | 0.002 | **0.013** | -2.5 | -0.0002 | 0 | -0.002 |
| | | | | | **x29.28** | | | | |
| arc1 slip x3 all-band **OFF** | 2.018 | +0 | 0.517 | 0.447 | **0.498** | +4.0 | -0.0891 | 0 | -0.001 |
| arc1 slip x3 all-band **ON** | 9.858 | +0 | 8.652 | 6.766 | **7.658** | +25.8 | -1.3016 | 0 | +0.003 |
| | | | | | **x0.06** | | | | |
| arc1 slip x1 control **OFF** | 0.054 | +11 | 0.051 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | -0.001 |
| arc1 slip x1 control **ON** | 0.054 | +11 | 0.051 | 0.037 | **0.044** | +1.4 | +0.0056 | 0 | -0.001 |
| | | | | | **x1.00** | | | | |

Per band on the all-band slip pair (floor +14..+45), which is where the fallback's
cost is visible:

| band | OFF (free all 3) | ON (keep L1C) | OFF/ON |
|---|---|---|---|
| L1C | 0.498 | 7.658 | x0.06 |
| L2W | 0.665 | 9.763 | x0.07 |
| L5Q | 0.700 | 10.197 | x0.07 |

### Z.4 Verdict

1. **`raim_erase_band_scope: min_keep_one` does what Study 1 predicted, on all
   three arcs.** arc 1's 0.445 floor becomes 0.049 — the 0.050 the scoped
   injector arm measured, reproduced by the production path. The slant-iono
   column is the direct witness and changes SIGN as well as magnitude
   (-0.083 -> +0.006 m at +45 on arc 1): with one band pinned the two re-tapped
   levels land on the survivor's ionosphere instead of on one epoch of code.
   The win is largest where the floor was cheapest (arc 2, 81.3 deg, x63) and
   still 29x on the 13.8 deg arc — this response does not care about elevation
   the way the unscoped one does, because it is no longer re-drawing (rho, I, N)
   from code at all.
2. **`cycle_slip.refloat_band_scope: implicated` is inert where it should be and
   harmful where the header warned it would be.** The single-band slip control is
   **identical to three decimals on every column** — the knob does not perturb a
   detector response that was already correctly scoped, which is the structural
   check. On a genuine all-band slip it is **15x worse**: the kept L1C really
   moved 10 cycles, the pin injects that into the LOS level, and the other two
   bands re-tap onto it (floor 0.498 -> 7.658, slant iono -0.089 -> -1.302 m,
   ZWD +4.0 -> +25.8 mm). This is the one case the design named as a deliberate
   trade; the trade is now PRICED, and at this price it is not worth taking.
3. **The trade is not symmetric, and that is the useful part.** A T4 attribution
   erase fires on a population that is overwhelmingly NOT slipped (the isolated-arc
   refloat population is FA-dominated — `SPEC_slip_relevel_walk` §6), so keeping a
   band costs nothing and saves the re-level. A detector refloat fires on positive
   raw evidence, so keeping a band is a bet against the detector. Same mechanism,
   opposite prior — which is why one knob is armed and the other is not.
4. **The open build is unchanged and now motivated by a number.** The honest
   handling of a whole-satellite loss of lock is neither "free every band" (0.498)
   nor "keep one and trust it" (7.658) but the LANE-PRESERVING refloat of
   `SPEC_lane_slip_response` §7 — re-level the kept band from the EWL-MW snap
   (sigma ~ 0.05 cyc) instead of carrying its pre-slip value. Until that exists,
   `refloat_band_scope` stays `all`.
5. **QC stays silent, again.** 0 post-event T4/T3 re-flags on the arc in every leg
   of the matrix, including the 7.7-cycle one. §9.6(5)'s finding is unchanged: an
   arc that has just been given a metre-class level error is not flagged for it.

### Z.6 Storm-bed validation — the combined arm FAILS, and Z.4 said which half

`config/test_suite/_golden_sim_campaign_zwdblk_bscope.json` = main's
`_golden_sim_campaign_zwdblk.json` + BOTH knobs (a 3-key diff: the two knobs and
the run path). 6 h, 60 solve stations + 1 held-out consumer, the SLIPPED campaign
bed `runs/simnet60_campaign_wu93` (2 486 truth slips), Release, exit 0 in 1 099.7 s
-> `C:/GnssProcessor/runs/golden_sim_campaign_zwdblk_bscope/`. Gates frozen before
the run.

| gate | zwdblk (reference, same commands) | bscope arm | gate | verdict |
|---|---|---|---|---|
| **ZWD settled truth rms** (`simnet_plateau_decompose` D0) | 1.1884 m | **0.8347 m** (-30 %) | <= 0.5 PASS, <= 0.9 PARTIAL | **PARTIAL** |
| **consumer 3D p50 / rms** (`simnet_grade`) | 1.0853 / 2.3890 m | **2.8920 / 3.0084 m** | p50 <= 0.6 AND rms <= 1.5 | **FAIL** (2.7x / 1.3x WORSE) |
| **ambient DD \|e\|** (`simnet_t4_census` control, n=3000) | 1.735 cyc | **2.974 cyc** | <= 0.5 | **FAIL** (1.7x WORSE) |
| **T4 events** (`simnet_t4_census`) | 9 871 | **9 181** (-7 %) | <= 5 000 | **FAIL** |
| **ref-free WL(1,2) at own integer, settled** (`simnet_amb_integer --ref-free`) | 86.2 % | **56.9 %** | >= 88 % | **FAIL** (-29 pts) |
| ref-free EWL(2,5), same window | 99.2 % | 88.5 % | — | worse |
| slip detection (`simnet_slip_census`) | 2337 det / 75 resp / 74 silent | 2338 / 78 / 70 | unchanged | unchanged |

Run-end counters (invariant #4 — the wiring is live, not inert):

```
T4 erases 9181 cell(s) -> 15323 band(s) erased / 9181 kept (4224 kept over gate,
0 single-band cells skipped); detector refloats 1091 arc(s) -> 1091 band(s) freed
/ 1091 kept (1091 all-band fallbacks; 0 pins on RAW MW/GF evidence, the rest
lexical; 0 single-band arcs; 0 TOO LATE).
```

**Reading.** The wire's tropospheric channel improves exactly as the mechanism
predicts — 30 % off the ZWD settled truth rms, the largest single move any arm has
made on that number since the ZWD-in-block card itself, and the hourly series falls
1.95 / 1.27 / 0.84 / 0.68 / 0.92 / 0.88 against 1.98 / 1.31 / 0.96 / 0.58 / 1.05 /
1.67, i.e. the late-run climb is gone. Everything that touches the INTEGERS is
worse, and the arm cannot attribute that between its two knobs.

**Z.4 already can, and it points at the detector half.** 1 091 all-band fallbacks
on a bed with 2 486 truth slips means the fallback pinned a band that really moved
roughly once per two slips — and Z.3 priced one such event at **15x** the level
floor of freeing all three (0.498 -> 7.658 cyc, slant iono -0.089 -> -1.302 m).
1 091 of those is the size of the observed integer collapse; nothing in the T4 half
(which Z.3 measured at 9x-64x BETTER, on a population that is FA-dominated by
construction) predicts a loss. Note also that **0 of the 1 091 pins were chosen on
raw MW/GF evidence** — every one fell to the lexical fallback, because a whole-arc
detector implication on this bed is an LLI-on-every-band case whose primary-pair
step is not usable. The detector half therefore ran, on this bed, as "keep L1C
whatever happened to it", which is the worst version of the trade.

**Verdict.** `network.raim_erase_band_scope: min_keep_one` is VALIDATED at leg
scale (Z.3, three arcs, 9x / 29x / 64x with the slant-iono witness) and is the
knob to promote; `network.cycle_slip.refloat_band_scope: implicated` is KEEP_OFF
— falsified at leg scale on a genuine all-band slip and consistent with the storm
arm's integer collapse. **The decisive follow-up is a T4-ONLY storm arm**
(`raim_erase_band_scope: min_keep_one`, `refloat_band_scope` left at `all`), which
the one-long-arm budget of this session did not allow; the prediction it tests is
ZWD at or better than 0.835 m with the consumer and the lanes at or better than
zwdblk's 1.085 / 86.2 %. **A quiet-bed 24 h check is also OWED and was NOT run
here** — the quiet null leg (Z.2) shows the detector scope acting 20x in 61 epochs
on a bed with no slips at all, so the quiet arm is not a formality.

**Caveat on the reference numbers.** The gates' "ambient 1.65 cyc" was
pre-registered from an earlier read; re-measured with the same command on the same
reference run it is 1.735 (strict variant 1.608). The arm is graded against the
re-measured value, and the direction of every gate is the same either way.

### Z.5 Reproducing

```powershell
python scripts\ev_study3_legs.py make
python scripts\ev_study3_legs.py run z_null_on z_a1erase_on z_slip3_on   # <=3 at a time
python scripts\ev_study3_legs.py run z_a2erase_on z_a3erase_on z_slip1_on
python scripts\ev_study3_legs.py run z_slip3_off
python scripts\ev_study3_table.py --bands
```

Wall clock: 137-192 s per leg with 3 concurrent, 7 legs in ~8 min; Release build
135-244 s, Debug suite 51 s. Unit suite after the card: **1738 cases / 244 suites,
1721 passed, 0 failed, 17 skipped** (fixture-gated; `data/` is not bootstrapped in
this worktree) — the card adds 22 cases in `tests/test_band_scoped_response.cpp`.

**Caveats.** (a) One quiet sim bed, three arcs, 61-epoch legs; the floor is
measured to +45, not to convergence. (b) The all-band leg's fallback cost is
measured on ONE arc with a 10/8/6-cycle slip; the cost scales with the kept band's
own slip, so it is a magnitude, not a constant. (c) The ON arms' background (Z.2)
is cancelled by grading against an ON null, not eliminated — an A/B that used the
OFF null would be measuring both. (d) The T4 scope was exercised here only through
INJECTED erases on healthy arcs; a real attribution on a genuinely bad arc is the
storm-bed arm below.
